// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_COMMAND_SERVICE_HPP
#define MUPDF_WORKER_COMMAND_SERVICE_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/document_base.hpp"
#include "engine/ocr/jobs.hpp"
#include "shared/model/types.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/transport/common.hpp"
#include "shared/transport/ctrl_channel.hpp"
#include "shared/transport/fd_channel.hpp"
#include "sys/sandbox.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

struct CmsResult;

}

namespace Mu::Worker::Runtime {

using ::Mu::Model::AnnotationAddRequest;
using ::Mu::Model::AnnotationModifyRequest;
using ::Mu::Model::AnnotationRemoveRequest;
using ::Mu::Model::DocumentType;
using ::Mu::Model::FontsRequest;
using ::Mu::Model::FormResetRequest;
using ::Mu::Model::FormUpdateRequest;
using ::Mu::Model::MetadataRequest;
using ::Mu::Model::OcrPageRequest;
using ::Mu::Model::OcrResult;
using ::Mu::Model::OcrResultRequest;
using ::Mu::Model::PageLinksNotification;
using ::Mu::Model::ReleaseFrameSlotRequest;
using ::Mu::Model::RenderRequest;
using ::Mu::Model::RequestMessage;
using ::Mu::Model::ResponseMessage;
using ::Mu::Model::SavePdfRequest;
using ::Mu::Model::SettingsRequest;
using ::Mu::Model::SignInput;
using ::Mu::Model::SignRequest;
using ::Mu::Model::TextBoxesRequest;

using ::Mu::IPC::CtrlChannel;
using ::Mu::IPC::FdChannel;
using ::Mu::Worker::Engine::DocumentBase;
using ::Mu::Worker::Engine::OcrJobs;
using Sandbox::Status;

// Resource limits protecting the worker process against DoS or memory exhaustion on untrusted input
constexpr std::size_t MaxResponseBoxes = 200'000;
constexpr std::size_t MaxResponseTextBytes = 16U * 1024U * 1024U;
constexpr std::size_t MaxOpenPages = 100'000;
constexpr std::size_t MaxOpenAnnotations = 100'000;
constexpr std::size_t MaxOpenSignatures = 100'000;
constexpr std::size_t MaxOpenLinks = 100'000;
constexpr std::size_t MaxEmbeddedFileBytes = 32U * 1024U * 1024U;
constexpr std::size_t MaxEmbeddedFileCount = 1'024;
constexpr std::size_t MaxOutlineResponseNodes = 50'000;
constexpr std::uint64_t FramePoolBytes = ::Mu::Limit::MaxSharedFrameBytes;
constexpr std::size_t MaxFramePoolSlots = 8;

// Deferred request queue limits during synchronous nested IPC loops
constexpr std::size_t MaxDeferredFrames = 1024;
constexpr std::size_t MaxDeferredBytes = 16U * 1024U * 1024U;

/// Session-scoped communication channels and sandbox status.
struct SessionContext {
    Sandbox::Status sandbox;
    FdChannel* fdChannel = nullptr;
    CtrlChannel* controlChannel = nullptr;
};

/**
 * Core command dispatch service executing protocol operations on document engines.
 *
 * Responsibilities:
 * 1. Dispatches all incoming IPC protocol requests to their target engine methods.
 * 2. Manages document lifecycle (PDF and EPUB opening, password unlocking, closing, memory trimming).
 * 3. Handles shared memory frame allocation (`memfd_create`), pixmap rendering, and descriptor handoff.
 * 4. Maintains stable opaque annotation handles mapping string tokens to PDF object numbers.
 * 5. Controls background multithreaded OCR task scheduling and notification queueing.
 *
 * The service may request session termination after an unrecoverable nested protocol failure, but the
 * WorkerServer owns the control-channel transport and performs the actual teardown.
 */
class CommandService {
public:
    explicit CommandService(SessionContext session = { });
    ~CommandService() = default;

    CommandService(const CommandService&) = delete;
    CommandService& operator=(const CommandService&) = delete;
    CommandService(CommandService&&) noexcept = delete;
    CommandService& operator=(CommandService&&) noexcept = delete;

    // -------------------------------------------------------------------------
    // IPC Deferred Message Queue
    // -------------------------------------------------------------------------

    /// Buffers an incoming raw message frame received during nested synchronous operations.
    /// Returns false if the queue capacity or byte limit is exceeded.
    [[nodiscard]] bool deferIncoming(std::vector<std::byte> raw);

    /// Dequeues the next deferred incoming message frame in FIFO order.
    [[nodiscard]] std::optional<std::vector<std::byte>> takeDeferredIncoming();

    // -------------------------------------------------------------------------
    // OCR & Background Notifications
    // -------------------------------------------------------------------------

    /// Returns the event file descriptor signaled when background OCR jobs complete.
    [[nodiscard]] int ocrCompletionFd() const noexcept;

    /// Drains all completed OCR job notifications from the queue.
    [[nodiscard]] std::vector<OcrJobs::Notification> drainOcrNotifications();

    /// Extracts one page per event-loop turn and returns a generation-tagged aggregate
    /// only after every page succeeds or a terminal error is reached.
    [[nodiscard]] std::optional<PageLinksNotification> processPageLinks();

    [[nodiscard]] bool hasPendingPageLinks() const noexcept { return m_pendingPageLinks.has_value(); }

    /// Cancels the current page-link aggregation and drops its PDF-only resolution cache.
    void cancelPageLinks() noexcept;

    /// Retrieves and removes the completed OCR result payload for a given job ID.
    [[nodiscard]] std::optional<OcrResult> takeOcrResult(std::uint64_t id);

    // -------------------------------------------------------------------------
    // Document Lifecycle & File Management
    // -------------------------------------------------------------------------

    /// Opens the sole document engine instance from an inherited open file
    /// descriptor. DocumentBase::openFd owns fd regardless of success/failure.
    [[nodiscard]] bool openFd(int fd,
                              std::string displayName,
                              DocumentType type = DocumentType::Pdf,
                              std::string* error = nullptr,
                              const std::vector<std::uint8_t>& epubAccelerator = { });

    /// Processes an OpenFd request and returns a ResponseMessage containing OpenResponse page geometries.
    [[nodiscard]] ResponseMessage openFdResponse(std::uint64_t requestId,
                                                 int fd,
                                                 std::string displayName,
                                                 const std::string& password = { },
                                                 DocumentType type = DocumentType::Pdf,
                                                 const std::vector<std::uint8_t>& epubAccelerator = { });

    [[nodiscard]] ResponseMessage
    openRequestResponse(std::uint64_t requestId, int fd, const Model::OpenRequest& request);

    /// Saves document modifications back to an output file descriptor.
    [[nodiscard]] ResponseMessage saveFdResponse(std::uint64_t id, int fd);

    /// Exports page subsets to a PDF output file descriptor.
    [[nodiscard]] ResponseMessage savePdfFdResponse(std::uint64_t id, const SavePdfRequest& payload, int fd);

    /// Performs digital signature signing on a document page field.
    [[nodiscard]] ResponseMessage signFdResponse(const RequestMessage& request, const SignRequest& sign, int fd);

    // -------------------------------------------------------------------------
    // Page Rendering & Memory Mapping
    // -------------------------------------------------------------------------

    /// Renders a document page into shared memory and transfers the frame descriptor over the FD channel.
    [[nodiscard]] ResponseMessage renderResponse(const RequestMessage& request, const RenderRequest& render);

    /// Releases a pooled render frame after its plugin-side QImage lease ends.
    [[nodiscard]] ResponseMessage releaseFrameSlotResponse(const RequestMessage& request,
                                                           const ReleaseFrameSlotRequest& release);

    /// Closes the current document and clears document-scoped credentials,
    /// handles, page-link work, and OCR results before another document can open.
    void closeDocument() noexcept;

    /// Returns a const pointer to the active document engine.
    [[nodiscard]] const ::Mu::Worker::Engine::DocumentBase* document() const noexcept;

    // -------------------------------------------------------------------------
    // Protocol Dispatcher
    // -------------------------------------------------------------------------

    /// Dispatches an incoming protocol request message to its specific handler method.
    [[nodiscard]] ResponseMessage dispatch(const RequestMessage& request);

    /// Reports whether a nested protocol failure requires the server to end this session.
    [[nodiscard]] bool disconnectRequested() const noexcept { return m_disconnectRequested; }

private:
    struct HandleLocation {
        int page = -1;
        std::int32_t objectNumber = -1;
    };

    struct FrameSlot {
        std::uint64_t id = 0;
        std::uint64_t leaseId = 0;
        std::uint64_t capacity = 0;
        Sys::FileDescriptor fd;
        Sys::Mapping mapping;
        bool leased = false;
    };

    /// Incremental page-link aggregation retained between event-loop turns.
    struct PendingPageLinks {
        std::uint64_t generation = 0;
        int nextPage = 0;
        std::size_t totalLinks = 0;
        std::vector<::Mu::Model::PageLinks> pages;
    };

    // Private Handler Methods
    [[nodiscard]] std::string annotationHandle(int page, std::int32_t objectNumber);
    [[nodiscard]] std::string formFieldHandle(int page, std::int32_t objectNumber);
    [[nodiscard]] bool hasOpenDocument() const noexcept;
    [[nodiscard]] ResponseMessage ping(std::uint64_t id) const;
    [[nodiscard]] ResponseMessage annotationAdd(const RequestMessage& request, const AnnotationAddRequest& add);
    [[nodiscard]] ResponseMessage annotationModify(const RequestMessage& request,
                                                   const AnnotationModifyRequest& modify);
    [[nodiscard]] ResponseMessage annotationRemove(const RequestMessage& request,
                                                   const AnnotationRemoveRequest& remove);
    [[nodiscard]] ResponseMessage ocrPage(const RequestMessage& request, const OcrPageRequest& ocrPage, int inputFd);
    [[nodiscard]] ResponseMessage ocrResult(const RequestMessage& request, const OcrResultRequest& ocrResult);
    [[nodiscard]] ResponseMessage textBoxes(const RequestMessage& request, const TextBoxesRequest& boxes);
    [[nodiscard]] ResponseMessage documentInfo(const RequestMessage& request, const MetadataRequest& metadata);
    [[nodiscard]] ResponseMessage synopsis(const RequestMessage& request);
    [[nodiscard]] ResponseMessage fonts(const RequestMessage& request, const FontsRequest& fonts);
    [[nodiscard]] ResponseMessage settings(const RequestMessage& request, const SettingsRequest& settings);
    [[nodiscard]] ResponseMessage embeddedFiles(const RequestMessage& request);
    [[nodiscard]] ResponseMessage formUpdate(const RequestMessage& request, const FormUpdateRequest& update);
    [[nodiscard]] ResponseMessage formReset(const RequestMessage& request, const FormResetRequest& reset);
    [[nodiscard]] Model::FormUpdateResponse
    formUpdateResponse(const std::vector<Engine::DocumentBase::FieldMutation>& mutations) const;
    /// Performs the synchronous CMS reply exchange used by the MuPDF signer.
    [[nodiscard]] ::Mu::Worker::Engine::CmsResult requestCmsSignature(const SignInput& input);
    /// Receives ownership of a transferred descriptor or writes an error response.
    [[nodiscard]] int
    receiveFd(std::uint64_t requestId, const char* method, std::uint64_t transferId, ResponseMessage* failureResponse);
    // Session context (borrowed channels & sandbox status); channel teardown belongs to WorkerServer.
    SessionContext m_session;

    // Active document engine instance (PDF or EPUB)
    std::unique_ptr<DocumentBase> m_document;
    // Latest document and EPUB layout settings, retained across document close.
    ::Mu::Model::DocumentSettings m_settings;

    // Document-scoped reusable render mappings. A slot is never overwritten
    // while its lease is held by a plugin QImage.
    std::vector<FrameSlot> m_frameSlots;
    std::uint64_t m_framePoolBytes = 0;

    // Stable annotation handle mapping (handle string -> { page, pdfObjectNumber })
    std::unordered_map<std::string, HandleLocation> m_annotationHandles;
    // Reverse lookup used to resolve canonical annotation state into handles.
    std::unordered_map<std::uint64_t, std::string> m_annotationObjectHandles;
    // Stable form field handle mapping (handle string -> { page, pdfObjectNumber })
    std::unordered_map<std::string, HandleLocation> m_formFieldHandles;
    // Reverse lookup used to translate canonical worker state into handles.
    std::unordered_map<std::uint64_t, std::string> m_formObjectHandles;

    // Async OCR job controller
    OcrJobs m_ocrJobs;

    // Monotonic counter sequences
    std::uint64_t m_annotationGeneration = 0;
    std::uint64_t m_linkGeneration = 0;
    std::uint64_t m_nextFrameTransferId = 1;
    std::uint64_t m_nextFrameSlotId = 1;
    std::uint64_t m_nextSignJobId = 0;

    // Buffer for requests received during synchronous nested operations
    std::deque<std::vector<std::byte>> m_deferredIncoming;
    std::size_t m_deferredIncomingBytes = 0;
    bool m_disconnectRequested = false;

    // Active document password
    std::string m_documentPassword;
    std::optional<PendingPageLinks> m_pendingPageLinks;
};

} // namespace Mu::Worker::Runtime

#endif
