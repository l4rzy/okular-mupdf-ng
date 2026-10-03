// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <array>
#include <cstdio>
#include <unistd.h>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

// =============================================================================
// Helper Functions for Safe File Writing & Cleanup
// =============================================================================

namespace {

/// Helper to replicate file contents from source to destination stream before incremental append.
bool copyFileContents(FILE* src, FILE* dst)
{
    if (!src || !dst)
        return false;
    if (::fseek(src, 0, SEEK_SET) != 0)
        return false;

    std::array<unsigned char, Constant::FileCopyChunkBytes> buffer;
    for (;;) {
        const std::size_t readCount = ::fread(buffer.data(), 1, buffer.size(), src);
        if (readCount > 0) {
            if (::fwrite(buffer.data(), 1, readCount, dst) != readCount)
                return false;
        }
        if (readCount < buffer.size()) {
            if (::ferror(src))
                return false;
            break;
        }
    }
    return ::fflush(dst) == 0;
}

/// Helper to safely close and release Fitz output stream without leaking file descriptors.
void closeAndDropOutput(fz_context* context, fz_output* volatile& output, FILE* volatile& file) noexcept
{
    // This helper is used only after a failed write. Closing output is best
    // effort; the original MuPDF/write error remains the caller's diagnostic.
    if (output) {
        fz_try(context)
        {
            fz_close_output(context, output);
        }
        fz_catch(context)
        {
        }
        fz_drop_output(context, output);
        output = nullptr;
        file = nullptr;
    }
    if (file) {
        ::fclose(file);
        file = nullptr;
    }
}

// Match MuPDF's Print visibility rules while retaining original appearance
// streams (the PDF drawing device cannot reproduce tiled patterns).
bool shouldPrintAnnotation(fz_context* context, pdf_document* document, pdf_annot* annotation)
{
    const int flags = pdf_annot_flags(context, annotation);
    if (!(flags & PDF_ANNOT_IS_PRINT) || (flags & (PDF_ANNOT_IS_HIDDEN | PDF_ANNOT_IS_INVISIBLE)))
        return false;
    const auto type = pdf_annot_type(context, annotation);
    if (type == PDF_ANNOT_POPUP || type == PDF_ANNOT_FILE_ATTACHMENT)
        return false;
    pdf_obj* object = pdf_annot_obj(context, annotation);
    if (type == PDF_ANNOT_WIDGET
        && (!pdf_dict_get_inheritable(context, object, PDF_NAME(FT))
            || !pdf_dict_get_inheritable(context, object, PDF_NAME(T))))
        return false;
    return !pdf_is_ocg_hidden(context, document, nullptr, "Print", pdf_dict_get(context, object, PDF_NAME(OC)));
}

// Normalize the few print-specific properties that MuPDF's baker does not
// handle. All mutations affect the export copy, never the live document.
void preparePrintablePages(fz_context* context, pdf_document* document)
{
    pdf_page* volatile page = nullptr;
    fz_buffer* volatile buffer = nullptr;
    pdf_obj* volatile appearanceCopy = nullptr;
    pdf_obj* volatile dictionary = nullptr;
    pdf_obj* volatile contents = nullptr;
    pdf_obj* volatile prefix = nullptr;
    pdf_obj* volatile suffix = nullptr;
    fz_try(context)
    {
        buffer = fz_new_buffer_from_copied_data(context, reinterpret_cast<const unsigned char*>("q\n"), 2);
        prefix = pdf_add_stream(context, document, buffer, nullptr, 0);
        fz_drop_buffer(context, buffer);
        buffer = nullptr;
        buffer = fz_new_buffer_from_copied_data(context, reinterpret_cast<const unsigned char*>("Q\n"), 2);
        suffix = pdf_add_stream(context, document, buffer, nullptr, 0);
        fz_drop_buffer(context, buffer);
        buffer = nullptr;
        const int count = pdf_count_pages(context, document);
        for (int index = 0; index < count; ++index) {
            pdf_obj* pageObject = pdf_lookup_page_obj(context, document, index);
            pdf_obj* annotations = pdf_dict_get(context, pageObject, PDF_NAME(Annots));
            const int annotationCount = pdf_array_len(context, annotations);
            if (annotationCount > static_cast<int>(Constant::MaxPageAnnotations))
                fz_throw(context, FZ_ERROR_LIMIT, "resource limit: print annotation count exceeded");
            // Popups are omitted from MuPDF's annotation iterator, but its
            // baker visits the raw array and must not paint their appearances.
            for (int annotation = 0; annotation < annotationCount; ++annotation) {
                pdf_obj* object = pdf_array_get(context, annotations, annotation);
                if (pdf_name_eq(context, pdf_dict_get(context, object, PDF_NAME(Subtype)), PDF_NAME(Popup)))
                    pdf_dict_put_int(context, object, PDF_NAME(F), PDF_ANNOT_IS_HIDDEN);
            }
            page = pdf_load_page(context, document, index);
            for (bool widgets : { false, true }) {
                for (pdf_annot* annot = widgets ? pdf_first_widget(context, page) : pdf_first_annot(context, page);
                     annot;
                     annot = widgets ? pdf_next_widget(context, annot) : pdf_next_annot(context, annot)) {
                    pdf_annot_request_synthesis(context, annot);
                }
            }
            pdf_update_page(context, page);
            for (bool widgets : { false, true }) {
                for (pdf_annot* annot = widgets ? pdf_first_widget(context, page) : pdf_first_annot(context, page);
                     annot;
                     annot = widgets ? pdf_next_widget(context, annot) : pdf_next_annot(context, annot)) {
                    pdf_obj* object = pdf_annot_obj(context, annot);
                    const int flags = pdf_annot_flags(context, annot);
                    if (!shouldPrintAnnotation(context, document, annot)) {
                        pdf_dict_put_int(context, object, PDF_NAME(F), flags | PDF_ANNOT_IS_HIDDEN);
                        continue;
                    }
                    pdf_obj* appearance = pdf_annot_ap(context, annot);
                    if (!appearance || !(flags & (PDF_ANNOT_IS_NO_ROTATE | PDF_ANNOT_IS_NO_ZOOM)))
                        continue;
                    // Fold MuPDF's display transform into a private appearance
                    // matrix/rectangle, so ordinary baking also honors these flags.
                    const fz_matrix matrix =
                        fz_concat(pdf_xobject_matrix(context, appearance), pdf_annot_transform(context, annot));
                    const fz_rect bounds = fz_transform_rect(pdf_xobject_bbox(context, appearance), matrix);
                    dictionary = pdf_copy_dict(context, appearance);
                    pdf_dict_put_matrix(context, dictionary, PDF_NAME(Matrix), matrix);
                    buffer = pdf_load_raw_stream(context, appearance);
                    appearanceCopy = pdf_add_stream(context, document, buffer, dictionary, 1);
                    pdf_dict_put(
                        context, pdf_dict_put_dict(context, object, PDF_NAME(AP), 1), PDF_NAME(N), appearanceCopy);
                    pdf_dict_put_rect(context, object, PDF_NAME(Rect), bounds);
                    pdf_drop_obj(context, appearanceCopy);
                    appearanceCopy = nullptr;
                    pdf_drop_obj(context, dictionary);
                    dictionary = nullptr;
                    fz_drop_buffer(context, buffer);
                    buffer = nullptr;
                }
            }
            // The baker repairs unbalanced q/Q operators. This extra pair also
            // prevents balanced page transforms/clips from affecting annotations.
            if (annotationCount != 0) {
                pdf_obj* object = pdf_lookup_page_obj(context, document, index);
                pdf_obj* original = pdf_dict_get(context, object, PDF_NAME(Contents));
                contents = pdf_new_array(context, document, 3);
                pdf_array_push(context, contents, prefix);
                if (pdf_is_array(context, original)) {
                    for (int stream = 0; stream < pdf_array_len(context, original); ++stream)
                        pdf_array_push(context, contents, pdf_array_get(context, original, stream));
                } else if (original) {
                    pdf_array_push(context, contents, original);
                }
                pdf_array_push(context, contents, suffix);
                pdf_dict_put(context, object, PDF_NAME(Contents), contents);
                pdf_drop_obj(context, contents);
                contents = nullptr;
            }
            fz_drop_page(context, reinterpret_cast<fz_page*>(page));
            page = nullptr;
        }
    }
    fz_always(context)
    {
        fz_drop_page(context, reinterpret_cast<fz_page*>(page));
        fz_drop_buffer(context, buffer);
        pdf_drop_obj(context, appearanceCopy);
        pdf_drop_obj(context, dictionary);
        pdf_drop_obj(context, contents);
        pdf_drop_obj(context, prefix);
        pdf_drop_obj(context, suffix);
    }
    fz_catch(context)
    {
        fz_rethrow(context);
    }
}

} // namespace

// =============================================================================
// Full Document Saving
// =============================================================================

// Consumes fd on every path. fz_output owns the FILE* after construction,
// so it is deliberately never fclose'd after fz_close_output().
bool PdfDocument::saveFd(int fd, std::string* error)
{
    if (fd < 0)
        return fail(error, "output FD is invalid");

    pdf_document* pdf = pdf_specifics(m_context, m_document);
    if (!m_document || m_locked || !pdf) {
        ::close(fd);
        return fail(error, "document cannot be saved");
    }

    // Drop the cached pages before pdf_update_open_pages()/serialization so the
    // write sees the same open-page set it would without the render cache.
    clearPageCache();

    FILE* volatile file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }

    fz_output* volatile output = nullptr;
    volatile bool saved = false;

    fz_try(m_context)
    {
        pdf_write_options options = pdf_default_write_options;

        // If possible, attempt incremental save to preserve digital signatures and structure
        if (m_input && pdf_can_be_saved_incrementally(m_context, pdf)) {
            if (copyFileContents(m_input, file)) {
                options.do_incremental = 1;
            } else {
                // Fail closed: the staged prefix may be partial, so a full
                // rewrite over it could publish a corrupt file. The plugin
                // discards its temp file and the destination stays untouched.
                fz_throw(m_context, FZ_ERROR_GENERIC, "could not stage source for incremental save");
            }
        }

        output = fz_new_output_with_file_ptr(m_context, file);
        // MuPDF output owns FILE* from this point and closes it during output
        // close. Null the local owner so failure cleanup cannot double-close it.
        pdf_update_open_pages(m_context, pdf);
        pdf_write_document(m_context, pdf, output, &options);

        fz_flush_output(m_context, output);
        if (::fflush(file) != 0 || ::fsync(::fileno(file)) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "failed to flush save output");

        fz_close_output(m_context, output);
        fz_drop_output(m_context, output);
        output = nullptr;
        file = nullptr;
        saved = true;
    }
    fz_catch(m_context)
    {
        closeAndDropOutput(m_context, output, file);
        fail(error, fz_caught_message(m_context));
    }

    return saved;
}

// Flatten a grafted catalog rather than serializing or baking the live document.
// The shared graft map preserves cyclic references, forms, outlines and resources,
// while reading the current objects includes edits that have not been saved yet.
bool PdfDocument::flattenPdfFd(int fd, const std::vector<int>& pages, std::string* error)
{
    return writeBakedPdf(fd, pages, false, error);
}

bool PdfDocument::writeBakedPdf(int fd, const std::vector<int>& pages, bool forPrinting, std::string* error)
{
    if (fd < 0)
        return fail(error, "output FD is invalid");
    if (!m_document || m_locked) {
        ::close(fd);
        return fail(error, forPrinting ? "document cannot be printed" : "document cannot be flattened");
    }
    if (pages.size() > static_cast<std::size_t>(m_pageCount)) {
        ::close(fd);
        return fail(error, "export page selection is too large");
    }
    for (const int page : pages) {
        if (page < 0 || page >= m_pageCount) {
            ::close(fd);
            return fail(error, "export page is out of range");
        }
    }

    FILE* volatile file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }
    fz_output* volatile output = nullptr;
    pdf_document* volatile copy = nullptr;
    pdf_graft_map* volatile map = nullptr;
    volatile bool saved = false;
    fz_try(m_context)
    {
        pdf_document* source = pdf_specifics(m_context, m_document);
        copy = pdf_create_document(m_context);
        copy->version = source->version;
        map = pdf_new_graft_map(m_context, copy);
        pdf_obj* sourceTrailer = pdf_trailer(m_context, source);
        if (!forPrinting && pdf_dict_getp(m_context, sourceTrailer, "Root/AcroForm/XFA"))
            fz_throw(m_context, FZ_ERROR_ARGUMENT, "XFA forms cannot be flattened");
        pdf_obj* copyTrailer = pdf_trailer(m_context, copy);
        if (forPrinting) {
            // Copy only selected pages and the form/color/layer dependencies
            // needed to bake them; document attachments and actions stay behind.
            pdf_obj* sourceRoot = pdf_dict_get(m_context, sourceTrailer, PDF_NAME(Root));
            pdf_obj* copyRoot = pdf_dict_get(m_context, copyTrailer, PDF_NAME(Root));
            const int count = pages.empty() ? m_pageCount : static_cast<int>(pages.size());
            for (int index = 0; index < count; ++index) {
                const int sourcePage = pages.empty() ? index : pages[static_cast<std::size_t>(index)];
                pdf_graft_mapped_page(m_context, map, index, source, sourcePage);
                pdf_obj* sourceObject = pdf_lookup_page_obj(m_context, source, sourcePage);
                pdf_obj* copyObject = pdf_lookup_page_obj(m_context, copy, index);
                for (pdf_obj* key : { PDF_NAME(Annots), PDF_NAME(Group) }) {
                    if (pdf_obj* value = pdf_dict_get(m_context, sourceObject, key))
                        pdf_dict_put_drop(m_context, copyObject, key, pdf_graft_mapped_object(m_context, map, value));
                }
            }
            for (pdf_obj* key : { PDF_NAME(AcroForm), PDF_NAME(OCProperties), PDF_NAME(OutputIntents) }) {
                if (pdf_obj* value = pdf_dict_get(m_context, sourceRoot, key))
                    pdf_dict_put_drop(m_context, copyRoot, key, pdf_graft_mapped_object(m_context, map, value));
            }
            preparePrintablePages(m_context, copy);
        } else {
            pdf_dict_put_drop(
                m_context,
                copyTrailer,
                PDF_NAME(Root),
                pdf_graft_mapped_object(m_context, map, pdf_dict_get(m_context, sourceTrailer, PDF_NAME(Root))));
            if (pdf_obj* info = pdf_dict_get(m_context, sourceTrailer, PDF_NAME(Info)))
                pdf_dict_put_drop(
                    m_context, copyTrailer, PDF_NAME(Info), pdf_graft_mapped_object(m_context, map, info));
            if (!pages.empty())
                pdf_rearrange_pages(
                    m_context, copy, static_cast<int>(pages.size()), pages.data(), PDF_CLEAN_STRUCTURE_KEEP);
        }
        pdf_bake_document(m_context, copy, 1, 1);

        // MuPDF can abandon a failed bake without rethrowing. Never publish an
        // interactive document as a successful flattened export. Links remain.
        if (pdf_dict_getp(m_context, pdf_trailer(m_context, copy), "Root/AcroForm"))
            fz_throw(m_context, FZ_ERROR_GENERIC, "form flattening did not complete");
        for (int page = 0; page < pdf_count_pages(m_context, copy); ++page) {
            pdf_obj* annots = pdf_dict_get(m_context, pdf_lookup_page_obj(m_context, copy, page), PDF_NAME(Annots));
            for (int index = 0; index < pdf_array_len(m_context, annots); ++index) {
                pdf_obj* annot = pdf_array_get(m_context, annots, index);
                if (!pdf_name_eq(m_context, pdf_dict_get(m_context, annot, PDF_NAME(Subtype)), PDF_NAME(Link)))
                    fz_throw(m_context, FZ_ERROR_GENERIC, "annotation flattening did not complete");
            }
            if (forPrinting)
                pdf_dict_del(m_context, pdf_lookup_page_obj(m_context, copy, page), PDF_NAME(Annots));
        }

        pdf_write_options options = pdf_default_write_options;
        options.do_garbage = 1;
        options.do_encrypt = PDF_ENCRYPT_NONE;
        output = fz_new_output_with_file_ptr(m_context, file);
        pdf_write_document(m_context, copy, output, &options);
        fz_flush_output(m_context, output);
        if (::fflush(file) != 0 || ::fsync(::fileno(file)) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "failed to flush PDF output");
        fz_close_output(m_context, output);
        fz_drop_output(m_context, output);
        output = nullptr;
        file = nullptr;
        saved = true;
    }
    fz_always(m_context)
    {
        pdf_drop_graft_map(m_context, map);
        pdf_drop_document(m_context, copy);
    }
    fz_catch(m_context)
    {
        closeAndDropOutput(m_context, output, file);
        fail(error, fz_caught_message(m_context));
    }
    return saved;
}

// =============================================================================
// Printing
// =============================================================================

bool PdfDocument::savePdfFd(int fd, const std::vector<int>& pages, std::string* error)
{
    return writeBakedPdf(fd, pages, true, error);
}

} // namespace Mu::Worker::Engine
