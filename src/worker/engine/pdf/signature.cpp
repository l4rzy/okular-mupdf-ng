// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "engine/signer.hpp"
#include "shared/logging.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

namespace {

struct RawSignatureField {
    int page = -1;
    std::int32_t objectNumber = 0;
    int annotationFlags = 0;
    bool readOnly = false;
    bool visible = false;
    bool signedField = false;
    std::int64_t signingSeconds = 0;
    fz_rect rectangle { };
    const char* partialName = nullptr;
    const char* signerName = nullptr;
    const char* reason = nullptr;
    const char* location = nullptr;
    const char* subFilter = nullptr;
    char* fullyQualifiedName = nullptr;
    char* contents = nullptr;
    std::size_t contentsSize = 0;
    std::int64_t byteRange[4] { };
    int byteRangeCount = 0;
};

void freeRawSignatureField(fz_context* context, RawSignatureField& field) noexcept
{
    if (field.fullyQualifiedName)
        fz_free(context, field.fullyQualifiedName);
    if (field.contents)
        fz_free(context, field.contents);
    field.fullyQualifiedName = nullptr;
    field.contents = nullptr;
}

void clearSignatureMutation(fz_context* context, pdf_page* page, pdf_annot* widget, bool created)
{
    if (created) {
        pdf_delete_annot(context, page, widget);
        return;
    }

    // pdf_sign_signature marks the field read-only before invoking the CMS
    // callback. If the callback fails, clear that transient bit first so the
    // normal MuPDF rollback path can restore the unsigned field.
    if (pdf_obj* field = pdf_annot_obj(context, widget)) {
        const int flags = pdf_field_flags(context, field);
        if ((flags & PDF_FIELD_IS_READ_ONLY) != 0) {
            pdf_dict_put_int(context, field, PDF_NAME(Ff), flags & ~PDF_FIELD_IS_READ_ONLY);
            pdf_dirty_obj(context, field);
        }
    }
    pdf_clear_signature(context, widget);
}

std::optional<SignatureField> extractSignatureField(fz_context* context,
                                                    pdf_annot* annotation,
                                                    pdf_document* pdfDocument,
                                                    const fz_rect& bounds,
                                                    std::int64_t fileSize,
                                                    std::size_t& totalCmsBytes)
{
    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    RawSignatureField raw;
    bool accepted = false;
    fz_var(raw);
    fz_var(accepted);

    fz_try(context)
    {
        pdf_obj* field = pdf_annot_obj(context, annotation);
        const bool signatureWidget = pdf_widget_type(context, annotation) == PDF_WIDGET_TYPE_SIGNATURE
            || (field && pdf_name_eq(context, pdf_dict_get_inheritable(context, field, PDF_NAME(FT)), PDF_NAME(Sig)));
        if (signatureWidget) {
            raw.page = pdf_lookup_page_number(context, pdfDocument, pdf_dict_get(context, field, PDF_NAME(P)));
            raw.objectNumber = pdf_to_num(context, field);
            raw.partialName = pdf_to_text_string(context, pdf_dict_get_inheritable(context, field, PDF_NAME(T)));
            raw.fullyQualifiedName = pdf_load_field_name(context, field);
            raw.annotationFlags = pdf_dict_get_int(context, field, PDF_NAME(F));
            raw.readOnly = (pdf_field_flags(context, field) & PDF_FIELD_IS_READ_ONLY) != 0
                || (raw.annotationFlags & PDF_ANNOT_IS_READ_ONLY) != 0;
            raw.visible =
                (raw.annotationFlags & (PDF_ANNOT_IS_INVISIBLE | PDF_ANNOT_IS_HIDDEN | PDF_ANNOT_IS_NO_VIEW)) == 0;
            raw.signedField = pdf_signature_is_signed(context, pdfDocument, field) != 0;
            raw.rectangle = pdf_bound_annot(context, annotation);

            if (raw.signedField) {
                // Parse signature dictionary (/V entry)
                pdf_obj* signature = pdf_dict_get_inheritable(context, field, PDF_NAME(V));
                raw.signerName = pdf_to_text_string(context, pdf_dict_get(context, signature, PDF_NAME(Name)));
                raw.reason = pdf_to_text_string(context, pdf_dict_get(context, signature, PDF_NAME(Reason)));
                raw.location = pdf_to_text_string(context, pdf_dict_get(context, signature, PDF_NAME(Location)));

                if (pdf_obj* date = pdf_dict_get(context, signature, PDF_NAME(M))) {
                    const std::int64_t seconds = pdf_to_date(context, date);
                    if (seconds > 0)
                        raw.signingSeconds = seconds;
                }

                if (pdf_obj* filter = pdf_dict_get(context, signature, PDF_NAME(SubFilter)))
                    raw.subFilter = pdf_to_name(context, filter);

                // Validate /ByteRange array to check whether the signature covers the entire file
                pdf_obj* byteRange = pdf_dict_get(context, signature, PDF_NAME(ByteRange));
                if (byteRange && pdf_is_array(context, byteRange) && pdf_array_len(context, byteRange) == 4) {
                    raw.byteRangeCount = 4;
                    for (int index = 0; index < raw.byteRangeCount; ++index)
                        raw.byteRange[index] = pdf_to_int64(context, pdf_array_get(context, byteRange, index));
                }
                // Resolve /V ourselves and refuse anything but a live
                // dictionary: a dangling vnum makes pdf_load_unencrypted_object
                // return garbage that pdf_signature_contents dereferences.
                // The null-safe resolver (unlike the loader) yields NULL for
                // uncommitted numbers, and direct dictionaries keep working.
                pdf_obj* vRef = pdf_dict_get_inheritable(context, field, PDF_NAME(V));
                pdf_obj* vObj = vRef ? pdf_resolve_indirect(context, vRef) : nullptr;
                if (vObj && pdf_is_dict(context, vObj)) {
                    // Avoid asking MuPDF to allocate an oversized /Contents string.
                    pdf_obj* contents = pdf_dict_get(context, signature, PDF_NAME(Contents));
                    const std::size_t remainingCmsBytes = totalCmsBytes < Constant::MaxPageSignatureCmsBytes
                        ? Constant::MaxPageSignatureCmsBytes - totalCmsBytes
                        : 0;
                    const std::size_t maxContentsBytes = std::min(Constant::MaxSignatureCmsBytes, remainingCmsBytes);
                    if (contents && pdf_is_string(context, contents)
                        && pdf_to_str_len(context, contents) <= maxContentsBytes) {
                        raw.contentsSize = pdf_signature_contents(context, pdfDocument, field, &raw.contents);
                    }
                }
            }
            accepted = true;
        }
    }
    fz_catch(context)
    {
        // A malformed signature widget is non-critical to page extraction;
        // skip it while retaining other valid fields.
    }

    if (!accepted) {
        freeRawSignatureField(context, raw);
        return std::nullopt;
    }

    try {
        SignatureField value;
        value.page = raw.page;
        value.objectNumber = raw.objectNumber;
        if (raw.partialName)
            value.partialName = raw.partialName;
        if (raw.fullyQualifiedName)
            value.fullyQualifiedName = raw.fullyQualifiedName;
        value.readOnly = raw.readOnly;
        value.visible = raw.visible;
        value.signedField = raw.signedField;
        value.certificateStatus = CertificateStatus::NotVerified;
        value.certificateStatusAtSigningTime = CertificateStatus::NotVerified;
        value.certificateStatusCurrent = CertificateStatus::NotVerified;
        value.signatureStatus = value.signedField ? SignatureStatus::NotVerified : SignatureStatus::NotFound;
        value.left = (raw.rectangle.x0 - bounds.x0) / width;
        value.top = (raw.rectangle.y0 - bounds.y0) / height;
        value.right = (raw.rectangle.x1 - bounds.x0) / width;
        value.bottom = (raw.rectangle.y1 - bounds.y0) / height;

        if (raw.signedField) {
            if (raw.signerName)
                value.signerName = raw.signerName;
            if (raw.reason)
                value.reason = raw.reason;
            if (raw.location)
                value.location = raw.location;
            if (raw.signingSeconds > 0)
                value.signingTime = { true, raw.signingSeconds * 1000 };
            if (raw.subFilter)
                value.subFilter = raw.subFilter;

            if (value.subFilter.find("sha256") != std::string::npos)
                value.hashAlgorithm = HashAlgorithm::Sha256;
            else if (value.subFilter.find("sha384") != std::string::npos)
                value.hashAlgorithm = HashAlgorithm::Sha384;
            else if (value.subFilter.find("sha512") != std::string::npos)
                value.hashAlgorithm = HashAlgorithm::Sha512;
            else if (value.subFilter.find("sha1") != std::string::npos)
                value.hashAlgorithm = HashAlgorithm::Sha1;
            else if (value.subFilter.find("md5") != std::string::npos)
                value.hashAlgorithm = HashAlgorithm::Md5;

            if (raw.byteRangeCount == 4) {
                for (const auto range : raw.byteRange)
                    value.byteRange.push_back(range);
                const auto validRange = value.byteRange[0] == 0 && value.byteRange[1] >= 0
                    && value.byteRange[2] >= value.byteRange[1] && value.byteRange[3] >= 0
                    && value.byteRange[2] <= fileSize && value.byteRange[3] <= fileSize - value.byteRange[2];
                value.signsTotalDocument =
                    validRange && fileSize > 0 && value.byteRange[2] + value.byteRange[3] == fileSize;
            }

            if (raw.contents && raw.contentsSize <= Constant::MaxSignatureCmsBytes
                && raw.contentsSize <= Constant::MaxPageSignatureCmsBytes - totalCmsBytes) {
                value.cmsSignature.assign(reinterpret_cast<std::uint8_t*>(raw.contents),
                                          reinterpret_cast<std::uint8_t*>(raw.contents) + raw.contentsSize);
                totalCmsBytes += raw.contentsSize;
            }
        }
        freeRawSignatureField(context, raw);
        return value;
    } catch (...) {
        freeRawSignatureField(context, raw);
        throw;
    }
}

} // namespace

// =============================================================================
// Incremental Digital Signing
// =============================================================================

// The original revision is copied to the output before MuPDF appends a signed
// incremental update, preserving any existing signatures.
bool PdfDocument::signFd(const Model::SignRequest& request,
                         CmsCallback callback,
                         int outputFd,
                         SigningResult* signingResult,
                         std::string* error)
{
    if (signingResult)
        *signingResult = SigningResult::GenericError;

    if (outputFd < 0 || request.page < 0 || request.page >= m_pageCount || request.certificateNickname.empty()) {
        if (outputFd >= 0)
            ::close(outputFd);
        return fail(error, "sign request is invalid");
    }

    if (!m_document || m_locked) {
        ::close(outputFd);
        return fail(error, "document cannot be signed");
    }

    // Incremental saving is required so existing signatures or revisions are not invalidated
    if (!pdf_can_be_saved_incrementally(m_context, pdf_specifics(m_context, m_document))) {
        ::close(outputFd);
        return fail(error, "document cannot be saved incrementally");
    }

    // MuPDF must seek back and read the serialized PDF to calculate the
    // signature byte ranges before invoking the CMS callback.
    // Adopt the caller-owned descriptor exactly once. From this point, every
    // failure path must close file, unless MuPDF output takes ownership below.
    FILE* file = ::fdopen(outputFd, "w+b");
    if (!file) {
        ::close(outputFd);
        return fail(error, "could not adopt output FD");
    }

    pdf_pkcs7_signer* signer = nullptr;
    pdf_page* nativePage = nullptr;
    pdf_annot* widget = nullptr;
    fz_image* graphic = nullptr;
    fz_output* output = nullptr;
    FILE* sourceCopy = nullptr;
    bool saved = false;
    bool createdWidget = false;
    bool widgetMutationComplete = false;

    try {
        signer = createSigner(request.certificateNickname, request.certificateSubjectCommonName, std::move(callback));
    } catch (...) {
        ::fclose(file);
        return fail(error, "could not create signer");
    }

    std::array<unsigned char, 65536> copyBuffer { };
    bool hadValue = false;
    int savedFlags = 0;
    fz_var(nativePage);
    fz_var(widget);
    fz_var(graphic);
    fz_var(output);
    fz_var(sourceCopy);
    fz_var(file);
    fz_var(saved);
    fz_var(createdWidget);
    fz_var(widgetMutationComplete);
    fz_var(hadValue);
    fz_var(savedFlags);

    fz_try(m_context)
    {
        if (!m_input)
            fz_throw(m_context, FZ_ERROR_GENERIC, "source PDF is unavailable");

        // Step 1: Duplicate source file descriptor and clone original revision into output file
        const int sourceFd = ::dup(::fileno(m_input));
        sourceCopy = sourceFd < 0 ? nullptr : ::fdopen(sourceFd, "rb");
        if (!sourceCopy) {
            if (sourceFd >= 0)
                ::close(sourceFd);
            fz_throw(m_context, FZ_ERROR_GENERIC, "could not duplicate source PDF");
        }

        if (::fseek(sourceCopy, 0, SEEK_SET) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "could not seek source PDF");

        for (;;) {
            const size_t count = ::fread(copyBuffer.data(), 1, copyBuffer.size(), sourceCopy);
            if (count && ::fwrite(copyBuffer.data(), 1, count, file) != count)
                fz_throw(m_context, FZ_ERROR_GENERIC, "could not copy source PDF");
            if (count < copyBuffer.size()) {
                if (::ferror(sourceCopy))
                    fz_throw(m_context, FZ_ERROR_GENERIC, "could not read source PDF");
                break;
            }
        }
        // The source duplicate is no longer needed after the initial revision
        // has been copied; clear its owner immediately after closing it.
        ::fclose(sourceCopy);
        sourceCopy = nullptr;

        if (::fflush(file) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "could not flush source PDF");

        // Step 2: Initialize PKCS#7 signer and resolve target signature widget
        auto* pdf = pdf_specifics(m_context, m_document);
        nativePage = pdf_load_page(m_context, pdf, request.page);

        if (request.existingFieldObjectNumber >= 0) {
            // Sign an existing unsigned signature form field widget
            for (pdf_annot* candidate = pdf_first_widget(m_context, nativePage); candidate;
                 candidate = pdf_next_widget(m_context, candidate)) {
                if (pdf_to_num(m_context, pdf_annot_obj(m_context, candidate)) == request.existingFieldObjectNumber) {
                    widget = pdf_keep_annot(m_context, candidate);
                    break;
                }
            }
            if (!widget)
                fz_throw(m_context, FZ_ERROR_GENERIC, "signature field no longer exists");
            if (pdf_widget_type(m_context, widget) != PDF_WIDGET_TYPE_SIGNATURE)
                fz_throw(m_context, FZ_ERROR_GENERIC, "target is not a signature field");
            if (pdf_signature_is_signed(m_context, pdf, pdf_annot_obj(m_context, widget)))
                fz_throw(m_context, FZ_ERROR_GENERIC, "signature field is already signed");
            if (pdf_field_flags(m_context, pdf_annot_obj(m_context, widget)) & PDF_FIELD_IS_READ_ONLY)
                fz_throw(m_context, FZ_ERROR_GENERIC, "signature field is read-only");
        } else {
            // Create a new visual signature field widget with an O(1) 6-digit hex timestamp suffix
            const auto suffix =
                static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) & 0xFFFFFF;
            char sigName[64] { };
            std::snprintf(sigName, sizeof(sigName), "OkularMuPDFSignature_%06x", suffix);
            widget = pdf_create_signature_widget(m_context, nativePage, sigName);
            pdf_dict_del(m_context, pdf_annot_obj(m_context, widget), PDF_NAME(Lock));
            createdWidget = true;
            const fz_rect bounds = pdf_bound_page(m_context, nativePage, FZ_CROP_BOX);
            const float width = bounds.x1 - bounds.x0, height = bounds.y1 - bounds.y0;
            pdf_set_annot_rect(m_context,
                               widget,
                               { bounds.x0 + static_cast<float>(request.rectangle.left * width),
                                 bounds.y0 + static_cast<float>(request.rectangle.top * height),
                                 bounds.x0 + static_cast<float>(request.rectangle.right * width),
                                 bounds.y0 + static_cast<float>(request.rectangle.bottom * height) });
        }

        // Optional: Load background image graphic if provided
        if (!request.backgroundImage.empty()) {
            fz_buffer* imgBuf = nullptr;
            fz_var(imgBuf);
            fz_try(m_context)
            {
                imgBuf = fz_new_buffer_from_copied_data(
                    m_context, request.backgroundImage.data(), request.backgroundImage.size());
                graphic = fz_new_image_from_buffer(m_context, imgBuf);
            }
            fz_always(m_context)
            {
                if (imgBuf)
                    fz_drop_buffer(m_context, imgBuf);
            }
            fz_catch(m_context)
            {
                graphic = nullptr;
            }
        }

        // Snapshot the field's pre-sign state; pdf_sign_signature() installs
        // /V (a new, uncommitted object) before invoking the CMS callback,
        // so a callback failure needs an explicit, verifiable undo. The
        // snapshot lives in function scope so the catch block below can
        // verify the rollback against it.
        fz_try(m_context)
        {
            if (pdf_obj* target = pdf_annot_obj(m_context, widget)) {
                hadValue = pdf_dict_get_inheritable(m_context, target, PDF_NAME(V)) != nullptr;
                savedFlags = pdf_field_flags(m_context, target);
            }
        }
        fz_catch(m_context)
        {
            // Fail closed before mutating: rethrow so the outer catch
            // releases the output file and the tail drops run.
            fz_rethrow(m_context);
        }

        // Step 3: Register signer with MuPDF and compute incremental write
        pdf_sign_signature(m_context,
                           widget,
                           signer,
                           PDF_SIGNATURE_DEFAULT_APPEARANCE,
                           graphic,
                           request.reason.empty() ? nullptr : request.reason.c_str(),
                           request.location.empty() ? nullptr : request.location.c_str());

        pdf_update_open_pages(m_context, pdf);
        output = fz_new_output_with_file_ptr(m_context, file);
        // Output now owns FILE* and closes it when the output is closed. Do not
        // fclose file again on success or in the catch cleanup below.
        file = nullptr;

        pdf_write_options options = pdf_default_write_options;
        options.do_incremental = 1;
        options.do_compress_images = 1;
        pdf_write_document(m_context, pdf, output, &options);

        fz_flush_output(m_context, output);
        fz_close_output(m_context, output);
        fz_drop_output(m_context, output);
        output = nullptr;
        saved = true;

        // Step 4: Cleanup in-memory document state after successful output serialization
        clearSignatureMutation(m_context, nativePage, widget, createdWidget);
        widgetMutationComplete = true;
    }
    fz_catch(m_context)
    {
        if (output) {
            fz_try(m_context)
            {
                fz_close_output(m_context, output);
            }
            fz_catch(m_context)
            {
            }
            fz_drop_output(m_context, output);
            output = nullptr;
        }
        if (file) {
            ::fclose(file);
            file = nullptr;
        }
        if (sourceCopy) {
            ::fclose(sourceCopy);
            sourceCopy = nullptr;
        }
        // MuPDF mutates the widget before invoking the CMS callback. Roll back
        // that in-memory mutation when serialization or the callback fails,
        // and verify the postcondition: a dangling /V here segfaults the
        // next pageDetails inside pdf_signature_contents.
        if (widget && nativePage && !widgetMutationComplete) {
            bool restored = false;
            fz_var(restored);
            fz_try(m_context)
            {
                clearSignatureMutation(m_context, nativePage, widget, createdWidget);
                if (pdf_obj* target = pdf_annot_obj(m_context, widget)) {
                    pdf_obj* current = pdf_dict_get_inheritable(m_context, target, PDF_NAME(V));
                    auto* pdf = pdf_specifics(m_context, m_document);
                    restored = createdWidget ? (target == nullptr)
                                             : (hadValue ? current != nullptr : current == nullptr)
                            && (!pdf || !pdf_signature_is_signed(m_context, pdf, target));
                } else {
                    restored = createdWidget; // deleted annot is gone: correct
                }
            }
            fz_catch(m_context)
            {
                restored = false;
            }
            if (!restored) {
                // Explicit surgical restore from the snapshot, then re-verify.
                bool repaired = false;
                fz_var(repaired);
                fz_try(m_context)
                {
                    if (pdf_obj* target = pdf_annot_obj(m_context, widget)) {
                        if (!hadValue && !createdWidget)
                            pdf_dict_del(m_context, target, PDF_NAME(V));
                        pdf_dict_put_int(m_context, target, PDF_NAME(Ff), savedFlags);
                        pdf_dirty_obj(m_context, target);
                        auto* pdf = pdf_specifics(m_context, m_document);
                        repaired = !pdf || !pdf_signature_is_signed(m_context, pdf, target);
                    } else {
                        repaired = createdWidget;
                    }
                }
                fz_catch(m_context)
                {
                    repaired = false;
                }
                if (!repaired) {
                    MU_LOG(critical, "Mu::Worker", "signature rollback failed; document state is tainted");
                    fail(error, "signature rollback failed after signing error");
                }
            }
        }

        // A tainted rollback above already set a specific error; keep it.
        if (!error || error->empty()) {
            MU_LOG(warning, "Mu::Worker", std::string("signing failed: ") + fz_caught_message(m_context));
            const CmsResult cms = signerResult(signer);
            if (signingResult && cms.result != SigningResult::Success)
                *signingResult = cms.result;
            fail(error, cms.details.empty() ? fz_caught_message(m_context) : cms.details.c_str());
        }
    }

    if (graphic)
        fz_drop_image(m_context, graphic);
    if (widget)
        pdf_drop_annot(m_context, widget);
    if (nativePage)
        pdf_drop_page(m_context, nativePage);
    if (signer)
        pdf_drop_signer(m_context, signer);

    if (saved && signingResult)
        *signingResult = SigningResult::Success;

    return saved;
}

// =============================================================================
// Signature Field Discovery & Inspection
// =============================================================================

std::vector<SignatureField> PdfDocument::extractPageSignatures(fz_page* nativePage, const fz_rect& bounds) const
{
    if (!m_hasAcroForm || !nativePage || bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0)
        return { };

    pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
    if (!pdfPage)
        return { };

    pdf_annot* firstWidget = pdf_first_widget(m_context, pdfPage);
    if (!firstWidget)
        return { };

    pdf_document* pdfDocument = pdf_specifics(m_context, m_document);
    if (!pdfDocument)
        return { };

    std::vector<SignatureField> result;
    std::size_t totalCmsBytes = 0;
    fz_try(m_context)
    {
        for (pdf_annot* annotation = firstWidget; annotation; annotation = pdf_next_widget(m_context, annotation)) {
            if (result.size() >= Constant::MaxPageSignatures)
                fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: page signature limit exceeded");

            if (auto field =
                    extractSignatureField(m_context, annotation, pdfDocument, bounds, m_sourceSize, totalCmsBytes))
                result.push_back(std::move(*field));
        }
    }
    fz_catch(m_context)
    {
        return { };
    }
    return result;
}

} // namespace Mu::Worker::Engine
