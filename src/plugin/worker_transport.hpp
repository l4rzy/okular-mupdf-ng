// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_WORKER_TRANSPORT_HPP
#define MU_PLUGIN_WORKER_TRANSPORT_HPP

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QRect>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plugin/signing_authorization.hpp"
#include "shared/logging.hpp"
#include "shared/model/types.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/transport/ctrl_channel.hpp"
#include "shared/transport/fd_channel.hpp"

namespace Mu::Plugin {

/** Owns the worker process, control socket, and FD channel on the transport
 * thread. All public methods run on that thread; WorkerClient marshals calls
 * to it. */
class WorkerTransport final : public QObject {
    Q_OBJECT

public:
    explicit WorkerTransport(QObject* parent = nullptr)
        : QObject(parent)
        , m_process(this)
    {
        connect(&m_process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                &WorkerTransport::finished);
    }

    ~WorkerTransport() override { stop(); }

    // These methods are invoked on the transport thread. The process, control
    // channel, FD channel, and temporary paths must never be accessed by the
    // generator thread directly.
    bool start(const QString& hint, const QStringList& tessDataDirectories, Model::PingResponse* workerInfo);
    void stop();
    bool isConnected() const;
    /// Resident memory of the worker process in bytes; nullopt unless the
    /// process is running and its status is readable.
    std::optional<quint64> workerMemoryBytes() const;
    /// Transfers the source and optional accelerator to the worker. Persistent
    /// cache policy belongs to the caller; returned bytes are opaque here.
    Model::OpenStatus open(const QString& path,
                           const QString& password,
                           QList<Model::PageInfo>* pages,
                           Model::DocumentType type = Model::DocumentType::Pdf,
                           const std::vector<std::uint8_t>& accelerator = { },
                           std::vector<std::uint8_t>* producedAccelerator = nullptr);
    Model::OpenStatus openData(const QByteArray& data,
                               const QString& password,
                               QList<Model::PageInfo>* pages,
                               Model::DocumentType type = Model::DocumentType::Pdf);
    /// Closes the open document and clears staged input. Abandons any
    /// in-flight background PDF export silently (result discarded, no signal).
    bool close();
    QImage render(int page,
                  int width,
                  int height,
                  const QRect& rect,
                  int cancellationFd = -1,
                  Model::RenderLinks* links = nullptr);
    Model::ResolvedLink resolveLink(const QString& uri);

    [[nodiscard]] quint64 documentEpoch() const noexcept { return m_documentEpoch.load(); }

    std::vector<Model::TextBox>
    getTextBoxesForPage(int page, qreal dpiX, qreal dpiY, bool skipAnnots = false, bool* success = nullptr);
    std::optional<quint64> startOcrPage(int page, const QString& language, int dpi);
    Model::OcrResult ocrResult(quint64 id);
    bool cancelOcrJobs();
    std::vector<Model::Font> fonts(int page);
    /// Empty collections are successful; nullopt preserves retrieval failure.
    std::optional<std::vector<Model::EmbeddedFile>> embeddedFiles();
    /// Empty outlines are successful; nullopt preserves retrieval failure.
    /// Disabling the PDF fallback also bypasses cached generated outlines.
    std::optional<std::vector<Model::OutlineNode>> synopsis(bool allowHeuristic = true,
                                                            bool* generatedSynopsis = nullptr);
    std::optional<Model::LayersResponse> layers();
    std::optional<Model::LayersResponse> setLayer(const Model::SetLayerRequest& request);
    Model::DocumentMetadata getDocumentInfo(const QStringList& keys);
    std::optional<Model::AnnotationHandle> addAnnotation(int page, const Model::Annotation& annotation);
    bool modifyAnnotation(int page, const QString& handle, const Model::Annotation& annotation, bool appearance);
    bool removeAnnotation(int page, const QString& handle);
    std::optional<std::vector<Model::OutputFormatInfo>> outputFormats();
    bool exportDocument(const QString& target, const Model::ExportOptions& options);
    /// Returns immediately; completion arrives via exportProgress.
    std::optional<quint64> startExport(const QString& target, const Model::ExportOptions& options);
    Model::SignResponse signToFile(Model::SignRequest request, const QString& password, const QString& target);
    std::optional<Model::FormUpdateResponse> updateForm(const Model::FormUpdateRequest& request);
    std::optional<Model::FormUpdateResponse> resetForm(const Model::FormResetRequest& request);
    std::optional<Model::FormUpdateResponse> clickFormButton(const Model::FormButtonClickRequest& request);
    bool setSettings(const Model::DocumentSettings& settings);

signals:
    void operationFailed(const QString& message, Model::ErrorCode code);
    void processExited(int);
    void ocrDone(quint64, int);
    void pageLinksReady(quint64, std::vector<Model::PageLinks>, bool, QString);
    void exportProgress(Model::ExportProgressNotification progress);

private:
    friend class TestWorkerTransport;

    bool canExport(const Model::ExportOptions& options, bool asynchronous);

    struct FrameSlotMapping;
    struct FrameLease;

    /// In-flight asynchronous export owned until the completion notification,
    /// the 60s timeout, or session teardown finalizes or discards it.
    struct PendingExport {
        std::unique_ptr<QTemporaryFile> file;
        QString target;
        quint64 jobId = 0;
    };

    /// Single completion exit: stops the timer, finalizes or discards the
    /// temporary file, clears the pending export, and emits the result signal.
    void completeExport(bool success, QString error);

    /// Opens an automatically removed output beside target for atomic publication.
    static std::unique_ptr<QTemporaryFile> createOutputFile(const QString& target);

    /// Flushes, fsyncs, and atomically renames completed output onto target.
    /// Failure retains automatic cleanup and fills error when provided.
    static bool finalizeOutputFile(QTemporaryFile& file, const QString& target, QString* error);

    // File-producing requests use a temporary file and rename it only after
    // the worker has completed, so a failed or interrupted export cannot
    // leave a partial destination file.
    template <class Payload> bool writeFile(Payload payload, const QString& target)
    {
        auto file = createOutputFile(target);
        if (!file)
            return false;
        const auto transfer = m_nextTransfer++;
        std::string e;
        if (!m_fd.send(transfer, file->handle(), &e)) {
            MU_LOG(warning, "Mu::Plugin", "could not send output FD for " + target.toStdString() + ": " + e);
            return false;
        }
        if constexpr (std::is_same_v<Payload, Model::ExportRequest>)
            payload.output.transferId = transfer;
        else
            payload.file.transferId = transfer;
        auto response = call(std::move(payload));
        if (!response) {
            MU_LOG(warning, "Mu::Plugin", "worker did not answer the write request for " + target.toStdString());
            return false;
        }
        if (response->error) {
            MU_LOG(warning,
                   "Mu::Plugin",
                   "worker failed to write " + target.toStdString() + ": " + response->error->message);
            return false;
        }
        QString error;
        if (!finalizeOutputFile(*file, target, &error)) {
            MU_LOG(warning,
                   "Mu::Plugin",
                   "could not finalize output for " + target.toStdString() + ": " + error.toStdString());
            return false;
        }
        return true;
    }

    /// Reopens the retained source with an independent offset for a background reader.
    [[nodiscard]] std::unique_ptr<QFile> openSourceInput() const;
    // A request may receive asynchronous notifications while its response is
    // pending; call() serializes that exchange and handles both message kinds.
    std::optional<Model::ResponseMessage> requestOcr(int page, const QString& language, int dpi);
    // Opt in only when the caller has no existing error presentation.
    std::optional<Model::ResponseMessage> call(Model::RequestPayload payload, bool reportFailure = false);
    // Reads inline notifications until the expected response arrives. A single
    // absolute deadline covers the entire receive loop; failures abort the session.
    std::optional<Model::ResponseMessage> receiveResponse(quint64 requestId,
                                                          std::chrono::steady_clock::time_point deadline);
    void processIncomingNotifications();
    bool handleNotification(const Model::NotificationMessage& notification, std::string* error = nullptr);
    static void cleanupFrameLease(void* data);
    void releaseFrameSlot(quint64 session, quint64 slotId, quint64 leaseId);
    static QString findBinary(const QString& hint);
    void finished(int code, QProcess::ExitStatus status);
    void abort();
    void clearDocumentState();
    void cleanupSession();

    QProcess m_process;
    IPC::CtrlChannel m_ctrl;
    IPC::FdChannel m_fd;
    // Sole-owner unique_ptrs (no QObject parent): m_notifier is recreated per
    // session around the live control FD; m_exportTimer is created lazily once.
    std::unique_ptr<QSocketNotifier> m_notifier;
    QString m_socketPath, m_fdSocketPath, m_tempPath;
    SigningAuthorization m_signingAuthorization;
    QString m_activeSignPassword;
    std::optional<PendingExport> m_export;
    std::unique_ptr<QTimer> m_exportTimer;
    std::unordered_map<std::uint64_t, std::shared_ptr<FrameSlotMapping>> m_frameSlots;
    quint64 m_nextId = 1, m_nextTransfer = 1, m_linkGeneration = 0;
    std::atomic<quint64> m_documentEpoch { 0 };
    quint64 m_frameSession = 0;
#ifdef MU_DEBUG_ENABLED
    std::optional<std::chrono::steady_clock::time_point> m_pageLinksStartedAt;
#endif
    bool m_intentionalStop = false;
    bool m_inFlight = false;
    // Retain the exact opened source for background jobs and PDF TOC identity
    // (also covers openData). The original pathname may be replaced or deleted.
    std::unique_ptr<QFile> m_sourceFile;
    int m_pdfPageCount = 0;
    std::optional<std::vector<Model::OutlineNode>> m_pdfSynopsis;
    bool m_pdfSynopsisGenerated = false;
};

} // namespace Mu::Plugin

#endif // MU_PLUGIN_WORKER_TRANSPORT_HPP
