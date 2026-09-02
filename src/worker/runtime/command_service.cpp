// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/command_service.hpp"
#include "runtime/render_budget.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

#include "engine/epub/document.hpp"
#include "engine/ocr/ocr.hpp"
#include "engine/pdf/document.hpp"
#include "engine/signer.hpp"
#include "shared/logging.hpp"
#include "shared/model/validation.hpp"
#include "shared/protocol/zpp_codec.hpp"
#include "shared/transport/compat.hpp"
#include "shared/transport/frame_buffer.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Runtime {

namespace {

bool isResourceLimitError(std::string_view error) noexcept
{
    return error.starts_with("resource limit:");
}

std::uint64_t objectKey(int page, std::int32_t objectNumber)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(page)) << 32)
        | static_cast<std::uint32_t>(objectNumber);
}

std::size_t formStringBytes(const ::Mu::Model::FormField& field)
{
    std::size_t bytes = field.partialName.size() + field.uiName.size() + field.fullyQualifiedName.size()
        + field.groupName.size() + field.text.size() + field.onState.size();
    for (const auto& choice : field.choices)
        bytes += choice.size();
    for (const auto& exportValue : field.exportValues)
        bytes += exportValue.size();
    return bytes;
}

} // namespace

using namespace ::Mu::Model;
namespace ZppCodec = ::Mu::IPC::ZppCodec;
using ::Mu::IPC::CtrlChannel;
using ::Mu::IPC::FdChannel;
using ::Mu::IPC::framePixelData;
using ::Mu::Worker::Engine::CmsResult;
using ::Mu::Worker::Engine::DocumentBase;
using ::Mu::Worker::Engine::OcrJobs;
using ::Mu::Worker::Engine::runOcr;
using ::Mu::Worker::Sys::createMemfd;
using ::Mu::Worker::Sys::Mapping;

namespace {

template <typename... Handlers> struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

template <typename... Handlers> Overloaded(Handlers...) -> Overloaded<Handlers...>;

// Helper to construct a successful ResponseMessage with payload
template <typename T> ResponseMessage success(std::uint64_t id, T payload)
{
    return { id, ResponsePayload { std::move(payload) }, std::nullopt };
}

// Helper to construct a successful empty ResponseMessage
inline ResponseMessage success(std::uint64_t id) noexcept
{
    return { id, std::monostate { }, std::nullopt };
}

// Helper to construct an error ResponseMessage with custom operation scope
inline ResponseMessage failure(std::uint64_t id, ErrorCode code, std::string operation, std::string message)
{
    return { id, std::monostate { }, Error { code, std::move(operation), std::move(message) } };
}

// Counts outline nodes recursively up to a safety limit to prevent recursion depth exhaustion
std::size_t countOutlineNodes(const std::vector<OutlineNode>& nodes, std::size_t limit)
{
    std::size_t count = 0;
    for (const auto& node : nodes) {
        if (++count > limit)
            return count;
        count += countOutlineNodes(node.children, limit - count);
        if (count > limit)
            return count;
    }
    return count;
}

// Validates OCR result size constraints before constructing the response
ResponseMessage makeOcrResponse(std::uint64_t id, const OcrResult& result)
{
    std::size_t bytes = 0;
    if (result.boxes.size() > MaxResponseBoxes)
        return failure(id, ErrorCode::ResourceLimit, "ocr", "OCR response exceeds its item limit");

    for (const auto& box : result.boxes) {
        if (box.text.size() > MaxResponseTextBytes - bytes)
            return failure(id, ErrorCode::ResourceLimit, "ocr", "OCR response exceeds its text limit");
        bytes += box.text.size();
    }
    return success(id, OcrResponse { result });
}

} // namespace

// Lifecycle & IPC Channel Setup
CommandService::CommandService(SessionContext session)
    : m_session(std::move(session))
{
}

// =============================================================================
// IPC Deferred Request Queue Management
// =============================================================================

bool CommandService::deferIncoming(std::vector<std::byte> raw)
{
    if (m_deferredIncoming.size() >= MaxDeferredFrames || raw.size() > MaxDeferredBytes - m_deferredIncomingBytes) {
        MU_LOG(warning, "Mu::Worker", "deferred request queue is full; failing nested operation");
        return false;
    }
    m_deferredIncomingBytes += raw.size();
    m_deferredIncoming.push_back(std::move(raw));
    return true;
}

std::optional<std::vector<std::byte>> CommandService::takeDeferredIncoming()
{
    if (m_deferredIncoming.empty())
        return std::nullopt;

    auto result = std::move(m_deferredIncoming.front());
    m_deferredIncoming.pop_front();
    m_deferredIncomingBytes -= result.size();
    return result;
}

// =============================================================================
// Document Lifecycle & File Management
// =============================================================================

bool CommandService::openFd(int fd,
                            std::string displayName,
                            DocumentType type,
                            std::string* error,
                            const std::vector<std::uint8_t>& epubAccelerator)
{
    if (type == DocumentType::Unknown) {
        ::close(fd);
        if (error)
            *error = "document type is unknown";
        return false;
    }
    if (epubAccelerator.size() > MaxEpubAcceleratorBytes) {
        ::close(fd);
        if (error)
            *error = "EPUB accelerator exceeds limit";
        return false;
    }

    // A session has exactly one document. Replacing it is a full document
    // boundary: no frame, handle, OCR job, password, or engine cache may
    // retain state from the previous document.
    closeDocument();
    m_annotationGeneration++;
    m_document.reset();

    // Instantiate appropriate engine backend
    const auto storeSize = static_cast<std::size_t>(m_settings.memoryCacheBytes);
    if (type == DocumentType::Epub)
        m_document = std::make_unique<Engine::EpubDocument>(storeSize);
    else
        m_document = std::make_unique<Engine::PdfDocument>(storeSize);

    m_document->setSettings(m_settings);
    // DocumentBase::openFd takes ownership of fd on every exit path.
    const bool opened = type == DocumentType::Epub && !epubAccelerator.empty()
        ? static_cast<Engine::EpubDocument*>(m_document.get())
              ->openFdWithAccelerator(fd, std::move(displayName), epubAccelerator, error)
        : m_document->openFd(fd, std::move(displayName), error);
    if (!opened)
        m_document.reset();
    return opened;
}

ResponseMessage CommandService::openFdResponse(std::uint64_t id,
                                               int fd,
                                               std::string displayName,
                                               const std::string& password,
                                               DocumentType type,
                                               const std::vector<std::uint8_t>& epubAccelerator)
{
    Sys::FileDescriptor ownedFd(fd);

    if (type == DocumentType::Unknown)
        return failure(id, ErrorCode::InvalidRequest, "open", "document type is unknown");

    std::string error;
    // Step 1: Open document via inherited file descriptor
    if (!openFd(ownedFd.release(), std::move(displayName), type, &error, epubAccelerator)) {
        m_documentPassword.clear();
        return failure(id, ErrorCode::InvalidRequest, "open", error);
    }

    // Step 2: Authenticate password if document is encrypted.
    // The locked state must be captured before unlock clears it; it drives
    // the "documentHasPassword" metadata reported to the generator.
    m_documentPassword = password;
    const bool passwordRequired = m_document->isLocked() && !password.empty();
    if (m_document->isLocked() && !m_document->unlock(password, &error)) {
        m_document->close();
        m_documentPassword.clear();
        return failure(id, ErrorCode::PermissionDenied, "open", "wrong password or document is locked");
    }
    m_document->setPasswordRequired(passwordRequired);

    // Step 3: Enforce safety limit on maximum supported page count
    if (m_document->pageCount() > static_cast<int>(MaxOpenPages)) {
        closeDocument();
        return failure(id, ErrorCode::ResourceLimit, "open", "document has too many pages");
    }

    OpenResponse output;
    const auto linkGeneration = ++m_linkGeneration;
    output.linkGeneration = linkGeneration;
    output.pages.reserve(static_cast<std::size_t>(m_document->pageCount()));
    std::size_t annotations = 0, signatures = 0, formFields = 0, formTextBytes = 0;

    // Step 4: Populate initial page descriptors, geometries, annotations, and signatures.
    // Links are resolved incrementally after the response is sent.
    for (int page = 0; page < m_document->pageCount(); ++page) {
        auto details = m_document->pageDetails(page, &error, false);
        if (!error.empty()) {
            closeDocument();
            return failure(
                id, isResourceLimitError(error) ? ErrorCode::ResourceLimit : ErrorCode::Internal, "open", error);
        }

        annotations += details.annotations.size();
        signatures += details.signatures.size();
        formFields += details.formFields.size();
        for (const auto& field : details.formFields) {
            const std::size_t fieldBytes = formStringBytes(field);
            if (formTextBytes > Limit::MaxAggregateFormTextBytes
                || fieldBytes > Limit::MaxAggregateFormTextBytes - formTextBytes) {
                closeDocument();
                return failure(id, ErrorCode::ResourceLimit, "open", "document form text size exceeded");
            }
            formTextBytes += fieldBytes;
        }
        if (annotations > MaxOpenAnnotations || signatures > MaxOpenSignatures
            || formFields > Limit::MaxOpenFormFields) {
            closeDocument();
            return failure(id, ErrorCode::ResourceLimit, "open", "document metadata exceeds its limit");
        }

        for (auto& annotation : details.annotations) {
            annotation.handle = annotationHandle(page, annotation.pdfObjectNumber);
            annotation.nativeIndex = annotation.pdfObjectNumber;
        }

        for (auto& field : details.formFields) {
            field.page = page;
            field.handle = formFieldHandle(page, field.pdfObjectNumber);
        }

        output.pages.push_back({ page,
                                 details.geometry,
                                 std::move(details.annotations),
                                 std::move(details.signatures),
                                 std::move(details.links),
                                 std::move(details.formFields) });
    }

    m_pendingPageLinks = PendingPageLinks { linkGeneration, 0, 0, { } };
    if (type == DocumentType::Epub) {
        auto* epub = static_cast<Engine::EpubDocument*>(m_document.get());
        std::string acceleratorError;
        output.epubAccelerator = epub->exportAccelerator(&acceleratorError);
        if (!acceleratorError.empty())
            MU_LOG(warning, "Mu::Worker::Epub", "could not export EPUB accelerator: " + acceleratorError);
    }
    return success(id, std::move(output));
}

ResponseMessage CommandService::openRequestResponse(std::uint64_t requestId, int fd, const OpenRequest& request)
{
    return openFdResponse(
        requestId, fd, request.displayName, request.password, request.documentType, request.epubAccelerator);
}

ResponseMessage CommandService::saveFdResponse(std::uint64_t id, int fd)
{
    Sys::FileDescriptor ownedFd(fd);
    if (!hasOpenDocument()) {
        return failure(id, ErrorCode::NotOpen, "save", "no document is open");
    }
    std::string error;
    if (!m_document->saveFd(ownedFd.release(), &error))
        return failure(id, ErrorCode::Internal, "save", error);

    return success(id);
}

ResponseMessage CommandService::savePdfFdResponse(std::uint64_t id, const SavePdfRequest& payload, int fd)
{
    Sys::FileDescriptor ownedFd(fd);
    if (!hasOpenDocument()) {
        return failure(id, ErrorCode::NotOpen, "save_pdf", "no document is open");
    }
    std::string error;
    if (!m_document->savePdfFd(ownedFd.release(), payload.pages, &error))
        return failure(id, ErrorCode::Internal, "save_pdf", error);

    return success(id);
}

void CommandService::closeDocument() noexcept
{
    // Closing is a document boundary. No password, opaque handle, deferred link,
    // OCR result, or frame pool may be reused by the next open.
    m_pendingPageLinks.reset();
    if (m_document)
        m_document->close();

    m_frameSlots.clear();
    m_framePoolBytes = 0;
    m_documentPassword.clear();
    m_annotationHandles.clear();
    m_annotationObjectHandles.clear();
    m_formFieldHandles.clear();
    m_formObjectHandles.clear();
    m_ocrJobs.cancelAll();
}

const DocumentBase* CommandService::document() const noexcept
{
    return m_document.get();
}

bool CommandService::hasOpenDocument() const noexcept
{
    return m_document && m_document->isOpen();
}

// =============================================================================
// Page Rendering & Shared Memory Frame Buffer Management
// =============================================================================

ResponseMessage CommandService::renderResponse(const RequestMessage& request, const RenderRequest& render)
{
    if (!hasOpenDocument())
        return failure(request.id, ErrorCode::NotOpen, "render", "no document is open");

    if (render.page < 0 || render.page >= m_document->pageCount()
        || !isValidRenderDimensions(render.width, render.height, render.tile.has_value()))
        return failure(request.id, ErrorCode::InvalidRequest, "render", "invalid page or dimensions");

    if (!m_session.fdChannel)
        return failure(request.id, ErrorCode::Unavailable, "render", "FD channel unavailable");

    if (render.tile) {
        const auto& t = *render.tile;
        if (!isValidRenderTile(render.width, render.height, t.x, t.y, t.width, t.height))
            return failure(request.id, ErrorCode::InvalidRequest, "render", "tile is outside the image");
    }

    // Fit oversized requests before allocation. The frame remains valid while
    // Okular scales the returned lower-resolution image into its original bounds.
    const auto fitted = fitRenderRequestToFrameBudget(render);
    std::optional<DocumentBase::RenderTile> tile;
    if (fitted.request.tile) {
        const auto& t = *fitted.request.tile;
        tile = DocumentBase::RenderTile { t.x, t.y, t.width, t.height };
    }

    const auto rw = fitted.frameWidth;
    const auto rh = fitted.frameHeight;
    const auto outputStride = fitted.frameStride;
    const auto dataSize = fitted.frameDataBytes;
    const auto total = sizeof(IPC::FrameBufferHeader) + dataSize;

    if (total > Limit::MaxSharedFrameBytes)
        return failure(request.id, ErrorCode::ResourceLimit, "render", "frame exceeds transfer limit");

    std::string error;
    FrameSlot* slot = nullptr;
    for (auto& candidate : m_frameSlots) {
        if (!candidate.leased && candidate.capacity >= total && (!slot || candidate.capacity < slot->capacity)) {
            slot = &candidate;
        }
    }

    bool newSlot = false;
    if (!slot && m_frameSlots.size() < MaxFramePoolSlots && total <= FramePoolBytes - m_framePoolBytes) {
        auto fd = createMemfd("mupdf-frame", total, &error);
        if (!fd)
            return failure(request.id, ErrorCode::ResourceLimit, "render", error);
        Mapping mapping(::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd->get(), 0), total);
        if (!mapping)
            return failure(request.id, ErrorCode::Internal, "render", "could not map frame");
        m_frameSlots.push_back({ m_nextFrameSlotId++, 0, total, std::move(*fd), std::move(mapping), false });
        m_framePoolBytes += total;
        slot = &m_frameSlots.back();
        newSlot = true;
    }

    // Pooled slots retain their descriptor and writable mapping between
    // renders. The transient fallback preserves the existing safe lifecycle
    // when every compatible slot is leased or either pool budget is full.
    std::optional<Sys::FileDescriptor> transientFrame;
    Mapping transientMapping;
    void* address = nullptr;
    if (slot) {
        address = slot->mapping.data();
    } else {
        transientFrame = createMemfd("mupdf-frame", total, &error);
        if (!transientFrame)
            return failure(request.id, ErrorCode::ResourceLimit, "render", error);
        transientMapping =
            Mapping(::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, transientFrame->get(), 0), total);
        if (!transientMapping)
            return failure(request.id, ErrorCode::Internal, "render", "could not map frame");
        address = transientMapping.data();
    }

    auto* header = static_cast<IPC::FrameBufferHeader*>(address);
    *header = { IPC::FRAME_SHM_MAGIC,           IPC::FRAME_SHM_VERSION, request.id, static_cast<std::uint32_t>(rw),
                static_cast<std::uint32_t>(rh), outputStride,           1,          { } };

    const auto discardNewSlot = [&] {
        if (!newSlot)
            return;
        m_framePoolBytes -= slot->capacity;
        m_frameSlots.pop_back();
        slot = nullptr;
    };

    // Step 3: Render raster pixmap into mapped SHM buffer
    if (!m_document->renderToBuffer({ fitted.request.page, fitted.request.width, fitted.request.height, tile },
                                    IPC::framePixelData(address),
                                    outputStride,
                                    &error)) {
        discardNewSlot();
        return failure(request.id, ErrorCode::Internal, "render", error.empty() ? "MuPDF returned no image" : error);
    }

    std::uint64_t transferId = 0;
    if (!slot || newSlot) {
        transferId = m_nextFrameTransferId++;
        const int fd = slot ? slot->fd.get() : transientFrame->get();
        if (!m_session.fdChannel->send(transferId, fd, &error)) {
            discardNewSlot();
            return failure(request.id, ErrorCode::Unavailable, "render", error);
        }
    }

    std::uint64_t slotId = 0;
    std::uint64_t leaseId = 0;
    if (slot) {
        slot->leased = true;
        slotId = slot->id;
        leaseId = ++slot->leaseId;
    }

    return success(request.id,
                   RenderResponse { { transferId,
                                      slotId,
                                      leaseId,
                                      static_cast<std::int32_t>(rw),
                                      static_cast<std::int32_t>(rh),
                                      static_cast<std::int32_t>(outputStride),
                                      1 } });
}

ResponseMessage CommandService::releaseFrameSlotResponse(const RequestMessage& request,
                                                         const ReleaseFrameSlotRequest& release)
{
    if (!release.slotId || !release.leaseId)
        return failure(request.id, ErrorCode::InvalidRequest, "release-frame-slot", "invalid slot lease");

    const auto slot = std::find_if(m_frameSlots.begin(), m_frameSlots.end(), [&](const FrameSlot& candidate) {
        return candidate.id == release.slotId;
    });
    if (slot == m_frameSlots.end() || !slot->leased || slot->leaseId != release.leaseId)
        return success(request.id);

    slot->leased = false;
    return success(request.id);
}

// =============================================================================
// Digital Signatures & Synchronous IPC Callback Loop
// =============================================================================

ResponseMessage CommandService::signFdResponse(const RequestMessage& request, const SignRequest& sign, int fd)
{
    Sys::FileDescriptor ownedFd(fd);
    if (!m_session.controlChannel) {
        return failure(request.id, ErrorCode::Unavailable, "sign", "control socket unavailable");
    }

    if (!m_document || sign.page < 0 || sign.page >= m_document->pageCount()
        || (sign.existingFieldObjectNumber < 0 && !isValidNormalizedRect(sign.rectangle))) {
        return failure(request.id, ErrorCode::InvalidRequest, "sign", "invalid page or rectangle");
    }

    // CMS signature callback bridging MuPDF signer to plugin NSS crypto engine.
    auto callback = [this](const std::array<std::uint8_t, 32>& digest, const std::string& nickname) {
        const auto jobId = ++m_nextSignJobId;
        return requestCmsSignature({ jobId, std::to_string(jobId), nickname, digest });
    };

    std::string error;
    SigningResult signingResult;
    if (!m_document->signFd(sign, std::move(callback), ownedFd.release(), &signingResult, &error)) {

        SigningResult resultStatus = SigningResult::GenericError;
        if (signingResult != SigningResult::GenericError) {
            resultStatus = signingResult;
        } else if (error == "signature field is already signed") {
            resultStatus = SigningResult::FieldAlreadySigned;
        }

        return success(request.id, SignResponse { resultStatus, error });
    }

    return success(request.id, SignResponse { });
}

::Mu::Worker::Engine::CmsResult CommandService::requestCmsSignature(const SignInput& input)
{
    std::string error;

    // The signer callback is synchronous: unrelated requests are deferred so
    // the control channel remains ordered while the parent produces CMS bytes.
    if (!ZppCodec::writeMessage(*m_session.controlChannel,
                                NotificationMessage { input },
                                IPC::Timeout::SignRoundTripMs,
                                &error,
                                "worker")) {
        return { SigningResult::GenericError, error, { } };
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(IPC::Timeout::SignRoundTripMs);
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            return { SigningResult::GenericError, "signing timed out", { } };

        std::vector<std::byte> raw;
        if (!readFrame(*m_session.controlChannel, &raw, static_cast<int>(remaining), &error))
            return { SigningResult::GenericError, error, { } };

        RequestMessage incoming;
        if (!ZppCodec::decode(raw, &incoming, &error))
            return { SigningResult::GenericError, error, { } };

        if (auto* reply = std::get_if<SignReply>(&incoming.payload)) {
            if (reply->jobId != input.jobId || reply->nonce != input.nonce || incoming.id == 0) {
                MU_LOG(critical,
                       "Mu::Worker",
                       "signing protocol violation: expected jobId=" + std::to_string(input.jobId)
                           + " nonce=" + input.nonce + " but received jobId=" + std::to_string(reply->jobId)
                           + " nonce=" + reply->nonce + "; requesting session disconnect");
                m_disconnectRequested = true;
                return { SigningResult::GenericError, "signing protocol violation: mismatched jobId or nonce", { } };
            }
            return { reply->result, reply->details, reply->cmsSignature };
        }
        if (!deferIncoming(std::move(raw))) {
            MU_LOG(critical, "Mu::Worker", "deferred request queue overflow; requesting session disconnect");
            m_disconnectRequested = true;
            return { SigningResult::GenericError, "deferred request queue is full", { } };
        }
    }
}

// =============================================================================
// Annotation Management & Handle Tracking
// =============================================================================

std::string CommandService::annotationHandle(int page, std::int32_t objectNumber)
{
    if (page < 0 || objectNumber <= 0)
        return { };

    const auto key = objectKey(page, objectNumber);
    if (const auto it = m_annotationObjectHandles.find(key); it != m_annotationObjectHandles.end())
        return it->second;

    auto h = "g" + std::to_string(m_annotationGeneration) + "-p" + std::to_string(page) + "-o"
        + std::to_string(objectNumber);
    m_annotationHandles.emplace(h, HandleLocation { page, objectNumber });
    m_annotationObjectHandles.emplace(key, h);
    return h;
}

std::string CommandService::formFieldHandle(int page, std::int32_t objectNumber)
{
    if (page < 0 || objectNumber <= 0)
        return { };

    const auto key = objectKey(page, objectNumber);
    if (const auto it = m_formObjectHandles.find(key); it != m_formObjectHandles.end())
        return it->second;

    auto h = "g" + std::to_string(m_annotationGeneration) + "-f" + std::to_string(page) + "-o"
        + std::to_string(objectNumber);
    m_formFieldHandles.emplace(h, HandleLocation { page, objectNumber });
    m_formObjectHandles.emplace(key, h);
    return h;
}

ResponseMessage CommandService::annotationAdd(const RequestMessage& r, const AnnotationAddRequest& a)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "annot_add", "no document is open");
    if (a.page < 0 || a.page >= m_document->pageCount())
        return failure(r.id, ErrorCode::InvalidRequest, "annot_add", "invalid annotation");
    std::string_view reason;
    if (!isValidAnnotation(a.annotation, &reason))
        return failure(r.id, ErrorCode::InvalidRequest, "annot_add", std::string(reason));

    std::string e;
    std::int32_t object = -1;
    if (!m_document->addAnnotation(a.page, a.annotation, &object, &e))
        return failure(r.id, ErrorCode::Internal, "annot_add", e);

    return success(r.id, AnnotationResponse { { annotationHandle(a.page, object) } });
}

ResponseMessage CommandService::annotationModify(const RequestMessage& r, const AnnotationModifyRequest& m)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "annot_modify", "no document is open");
    const auto it = m_annotationHandles.find(m.mutation.handle.value);
    if (it == m_annotationHandles.end() || it->second.page != m.mutation.page)
        return failure(r.id, ErrorCode::InvalidRequest, "annot_modify", "invalid annotation handle");
    std::string_view reason;
    if (!isValidAnnotation(m.mutation.annotation, &reason))
        return failure(r.id, ErrorCode::InvalidRequest, "annot_modify", std::string(reason));

    std::string e;
    if (!m_document->modifyAnnotation(
            m.mutation.page, it->second.objectNumber, m.mutation.annotation, m.mutation.appearanceChanged, &e))
        return failure(r.id, ErrorCode::Internal, "annot_modify", e);

    return success(r.id);
}

ResponseMessage CommandService::annotationRemove(const RequestMessage& r, const AnnotationRemoveRequest& x)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "annot_remove", "no document is open");
    const auto it = m_annotationHandles.find(x.handle.value);
    if (it == m_annotationHandles.end() || it->second.page != x.page)
        return failure(r.id, ErrorCode::InvalidRequest, "annot_remove", "invalid annotation handle");

    std::string e;
    if (!m_document->removeAnnotation(x.page, it->second.objectNumber, &e))
        return failure(r.id, ErrorCode::Internal, "annot_remove", e);

    m_annotationObjectHandles.erase(objectKey(x.page, it->second.objectNumber));
    m_annotationHandles.erase(it);
    return success(r.id);
}

ResponseMessage CommandService::formUpdate(const RequestMessage& r, const FormUpdateRequest& u)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "form_update", "no document is open");

    const auto it = m_formFieldHandles.find(u.handle);
    if (it == m_formFieldHandles.end())
        return failure(r.id, ErrorCode::InvalidRequest, "form_update", "invalid form field handle");

    const int targetPage = it->second.page;
    const std::int32_t targetObject = it->second.objectNumber;

    std::vector<Engine::DocumentBase::FieldMutation> mutations;
    std::string error;
    if (!m_document->updateFormField(targetPage, targetObject, u.value, &mutations, &error))
        return failure(r.id, ErrorCode::Internal, "form_update", error);

    return success(r.id, formUpdateResponse(mutations));
}

ResponseMessage CommandService::formReset(const RequestMessage& r, const FormResetRequest& reset)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "form_reset", "no document is open");

    const auto it = m_formFieldHandles.find(reset.handle);
    if (it == m_formFieldHandles.end())
        return failure(r.id, ErrorCode::InvalidRequest, "form_reset", "invalid form field handle");

    std::vector<Engine::DocumentBase::FieldMutation> mutations;
    std::string error;
    if (!m_document->resetForm(it->second.page, it->second.objectNumber, &mutations, &error))
        return failure(r.id, ErrorCode::Internal, "form_reset", error);

    return success(r.id, formUpdateResponse(mutations));
}

FormUpdateResponse
CommandService::formUpdateResponse(const std::vector<Engine::DocumentBase::FieldMutation>& mutations) const
{
    FormUpdateResponse response;
    for (const auto& mutation : mutations) {
        const auto handle = m_formObjectHandles.find(objectKey(mutation.page, mutation.objectNumber));
        if (handle != m_formObjectHandles.end())
            response.affectedFields.push_back({ handle->second, mutation.actualValue });
        if (std::find(response.affectedPages.begin(), response.affectedPages.end(), mutation.page)
            == response.affectedPages.end()) {
            response.affectedPages.push_back(mutation.page);
        }
    }

    return response;
}

// =============================================================================
// OCR Processing
// =============================================================================

int CommandService::ocrCompletionFd() const noexcept
{
    return m_ocrJobs.eventFd();
}

std::vector<OcrJobs::Notification> CommandService::drainOcrNotifications()
{
    return m_ocrJobs.drainNotifications();
}

std::optional<PageLinksNotification> CommandService::processPageLinks()
{
    if (!m_pendingPageLinks || !hasOpenDocument())
        return std::nullopt;

    auto& pending = *m_pendingPageLinks;
    PageLinksNotification notification;
    notification.generation = pending.generation;

    // Resolve one page per turn so large documents do not monopolize the control
    // loop. A notification is sent only once this generation is complete or fails.
    std::string error;
    auto links = m_document->extractLinks(pending.nextPage, &error);
    if (!error.empty()) {
        notification.resourceLimited = isResourceLimitError(error);
        notification.error = std::move(error);
        cancelPageLinks();
    } else if (links.size() > MaxOpenLinks || pending.totalLinks > MaxOpenLinks - links.size()) {
        notification.resourceLimited = true;
        notification.error = "document link metadata exceeds its limit";
        cancelPageLinks();
    } else {
        pending.totalLinks += links.size();
        pending.pages.push_back({ pending.nextPage, std::move(links) });
        ++pending.nextPage;
        if (pending.nextPage < m_document->pageCount())
            return std::nullopt;

        notification.pages = std::move(pending.pages);
        cancelPageLinks();
    }

    return notification;
}

void CommandService::cancelPageLinks() noexcept
{
    m_pendingPageLinks.reset();
    // Link destinations are cached only while constructing this aggregate. Keeping
    // them after cancellation or delivery would retain document-specific metadata.
    if (auto* pdfDocument = dynamic_cast<Engine::PdfDocument*>(m_document.get()))
        pdfDocument->discardResolvedLinkCache();
}

std::optional<OcrResult> CommandService::takeOcrResult(std::uint64_t id)
{
    return m_ocrJobs.take(id);
}

ResponseMessage CommandService::ocrPage(const RequestMessage& r, const OcrPageRequest& o, int inputFd)
{
    Sys::FileDescriptor ownedFd(inputFd);
    if (!hasOpenDocument()) {
        return failure(r.id, ErrorCode::NotOpen, "ocr_page", "no document is open");
    }
    if (dynamic_cast<Engine::EpubDocument*>(m_document.get())) {
        return failure(r.id, ErrorCode::Unavailable, "ocr_page", "OCR is only supported for PDF documents");
    }

    if (o.page < 0 || o.page >= m_document->pageCount() || !isValidOcrDpi(static_cast<float>(o.dpi))) {
        return failure(r.id, ErrorCode::InvalidRequest, "ocr_page", "invalid page or DPI");
    }

    auto language = o.language.empty() ? std::string("eng") : o.language;
    if (o.asynchronous) {
        // Enqueue task into background worker thread pool
        auto job = m_ocrJobs.submit(ownedFd.release(), m_documentPassword, o.page, language, static_cast<float>(o.dpi));
        if (!job) {
            return failure(r.id, ErrorCode::ResourceLimit, "ocr_page", "too many jobs");
        }
        return success(r.id, JobResponse { *job });
    }

    // Synchronous execution path
    return makeOcrResponse(r.id,
                           runOcr(ownedFd.release(), m_documentPassword, o.page, language, static_cast<float>(o.dpi)));
}

ResponseMessage CommandService::ocrResult(const RequestMessage& r, const OcrResultRequest& o)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "ocr_result", "no document is open");
    auto result = takeOcrResult(o.jobId);
    if (!result)
        return failure(r.id, ErrorCode::Unavailable, "ocr_result", "job is not complete");

    return makeOcrResponse(r.id, *result);
}

// =============================================================================
// Document Metadata & Information Queries
// =============================================================================

ResponseMessage CommandService::ping(std::uint64_t id) const
{
    return success(id, PingResponse { std::string(::Mu::IPC::COMPAT), ::getpid(), m_session.sandbox });
}

ResponseMessage CommandService::textBoxes(const RequestMessage& r, const TextBoxesRequest& b)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "text_boxes", "no document is open");
    if (b.page < 0 || !isValidDpi(b.dpiX, b.dpiY))
        return failure(r.id, ErrorCode::InvalidRequest, "text_boxes", "invalid page or DPI");

    std::string e;
    auto values = m_document->textBoxes(b.page, b.dpiX, b.dpiY, MaxResponseBoxes, b.skipAnnots, &e);
    if (!e.empty())
        return failure(r.id, ErrorCode::Internal, "text_boxes", e);

    return success(r.id, TextBoxesResponse { std::move(values) });
}

ResponseMessage CommandService::documentInfo(const RequestMessage& r, const MetadataRequest& m)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "metadata", "no document is open");

    std::string e;
    auto value = m_document->metadata(m.keys, &e);
    if (!e.empty())
        return failure(r.id, ErrorCode::Internal, "metadata", e);

    return success(r.id, MetadataResponse { std::move(value) });
}

ResponseMessage CommandService::synopsis(const RequestMessage& r)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "synopsis", "no document is open");

    std::string e;
    auto nodes = m_document->outline(&e);
    if (!e.empty())
        return failure(r.id, isResourceLimitError(e) ? ErrorCode::ResourceLimit : ErrorCode::Internal, "synopsis", e);

    if (countOutlineNodes(nodes, MaxOutlineResponseNodes) > MaxOutlineResponseNodes)
        return failure(r.id, ErrorCode::ResourceLimit, "synopsis", "outline exceeds its limit");

    return success(r.id, OutlineResponse { std::move(nodes) });
}

ResponseMessage CommandService::fonts(const RequestMessage& r, const FontsRequest& f)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "fonts", "no document is open");

    std::vector<int> pages;
    if (f.page < 0) {
        for (int i = 0; i < m_document->pageCount(); ++i)
            pages.push_back(i);
    } else if (f.page < m_document->pageCount()) {
        pages.push_back(f.page);
    } else {
        return failure(r.id, ErrorCode::InvalidRequest, "fonts", "invalid page");
    }

    std::string e;
    auto values = m_document->fonts(pages, &e);
    if (!e.empty())
        return failure(r.id, ErrorCode::Internal, "fonts", e);

    return success(r.id, FontsResponse { std::move(values) });
}

ResponseMessage CommandService::settings(const RequestMessage& r, const SettingsRequest& s)
{
    m_settings = s.settings;
    if (m_document)
        m_document->setSettings(s.settings);

    return success(r.id);
}

ResponseMessage CommandService::embeddedFiles(const RequestMessage& r)
{
    if (!hasOpenDocument())
        return failure(r.id, ErrorCode::NotOpen, "embedded_files", "no document is open");
    std::string e;
    bool limit = false;
    auto files = m_document->embeddedFiles(MaxEmbeddedFileBytes, MaxEmbeddedFileCount, &limit, &e);
    if (!e.empty())
        return failure(r.id, ErrorCode::Internal, "embedded_files", e);

    if (limit)
        return failure(r.id, ErrorCode::ResourceLimit, "embedded_files", "attachments exceed transfer limit");

    return success(r.id, EmbeddedFilesResponse { std::move(files) });
}

// =============================================================================
// Main Request Dispatcher
// =============================================================================

int CommandService::receiveFd(std::uint64_t requestId,
                              const char* method,
                              std::uint64_t transferId,
                              ResponseMessage* failureResponse)
{
    if (!m_session.fdChannel || transferId == 0) {
        *failureResponse = failure(requestId, ErrorCode::InvalidRequest, method, "invalid file transfer");
        return -1;
    }
    std::string e;
    const int fd = m_session.fdChannel->receive(transferId, &e);
    if (fd < 0) {
        *failureResponse = failure(requestId, ErrorCode::InvalidRequest, method, e);
        return -1;
    }
    return fd;
}

ResponseMessage CommandService::dispatch(const RequestMessage& request)
{
    const auto dispatchWithFd = [&](const char* method, std::uint64_t transferId, auto&& handler) {
        ResponseMessage err;
        const int fd = receiveFd(request.id, method, transferId, &err);
        if (fd < 0)
            return err;
        return handler(fd);
    };

    return std::visit(
        Overloaded {
            [&](const PingRequest&) { return ping(request.id); },
            [&](const CloseRequest&) {
                closeDocument();
                return success(request.id);
            },
            [&](const OpenRequest& payload) {
                return dispatchWithFd("open", payload.file.transferId, [&](int fd) {
                    return openRequestResponse(request.id, fd, payload);
                });
            },
            [&](const SaveRequest& payload) {
                return dispatchWithFd(
                    "save", payload.file.transferId, [&](int fd) { return saveFdResponse(request.id, fd); });
            },
            [&](const SavePdfRequest& payload) {
                return dispatchWithFd("save_pdf", payload.file.transferId, [&](int fd) {
                    return savePdfFdResponse(request.id, payload, fd);
                });
            },
            [&](const SignRequest& payload) {
                return dispatchWithFd(
                    "sign", payload.file.transferId, [&](int fd) { return signFdResponse(request, payload, fd); });
            },
            [&](const RenderRequest& payload) { return renderResponse(request, payload); },
            [&](const ReleaseFrameSlotRequest& payload) { return releaseFrameSlotResponse(request, payload); },
            [&](const AnnotationAddRequest& payload) { return annotationAdd(request, payload); },
            [&](const AnnotationModifyRequest& payload) { return annotationModify(request, payload); },
            [&](const AnnotationRemoveRequest& payload) { return annotationRemove(request, payload); },
            [&](const OcrPageRequest& payload) {
                return dispatchWithFd(
                    "ocr_page", payload.file.transferId, [&](int fd) { return ocrPage(request, payload, fd); });
            },
            [&](const OcrResultRequest& payload) { return ocrResult(request, payload); },
            [&](const CancelOcrJobsRequest&) {
                m_ocrJobs.cancelAll();
                return success(request.id);
            },
            [&](const TextBoxesRequest& payload) { return textBoxes(request, payload); },
            [&](const MetadataRequest& payload) { return documentInfo(request, payload); },
            [&](const SynopsisRequest&) { return synopsis(request); },
            [&](const FontsRequest& payload) { return fonts(request, payload); },
            [&](const SettingsRequest& payload) { return settings(request, payload); },
            [&](const EmbeddedFilesRequest&) { return embeddedFiles(request); },
            [&](const FormUpdateRequest& payload) { return formUpdate(request, payload); },
            [&](const FormResetRequest& payload) { return formReset(request, payload); },
            [&](const SignReply&) {
                return failure(
                    request.id, ErrorCode::InvalidRequest, "request", "sign replies are only valid as nested messages");
            },
        },
        request.payload);
}

} // namespace Mu::Worker::Runtime
