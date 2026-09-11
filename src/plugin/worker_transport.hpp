// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_WORKER_TRANSPORT_HPP
#define MU_PLUGIN_WORKER_TRANSPORT_HPP

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
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
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/logging.hpp"
#include "shared/model/types.hpp"
#include "shared/transport/ctrl_channel.hpp"
#include "shared/transport/fd_channel.hpp"

#ifdef MU_DEBUG_ENABLED
#include <chrono>
#endif

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
    Model::OpenStatus open(const QString& path,
                           const QString& password,
                           QList<Model::PageInfo>* pages,
                           Model::DocumentType type = Model::DocumentType::Pdf);
    Model::OpenStatus openData(const QByteArray& data,
                               const QString& password,
                               QList<Model::PageInfo>* pages,
                               Model::DocumentType type = Model::DocumentType::Pdf);
    /// Closes the open document and clears staged input. Abandons any
    /// in-flight background PDF export silently (result discarded, no signal).
    bool close();
    QImage render(int page, int width, int height, const QRect& rect);
    std::vector<Model::TextBox> getTextBoxesForPage(int page, qreal x, qreal y, bool skipAnnots = false);
    std::optional<quint64> startOcrPage(int page, const QString& language, int dpi);
    Model::OcrResult ocrResult(quint64 id);
    bool cancelOcrJobs();
    std::vector<Model::Font> fonts(int page);
    std::vector<Model::EmbeddedFile> embeddedFiles();
    std::vector<Model::OutlineNode> synopsis();
    Model::DocumentMetadata getDocumentInfo(const QStringList& keys);
    std::optional<Model::AnnotationHandle> addAnnotation(int page, const Model::Annotation& annotation);
    bool modifyAnnotation(int page, const QString& handle, const Model::Annotation& annotation, bool appearance);
    bool removeAnnotation(int page, const QString& handle);
    bool saveToFile(const QString& target);
    bool savePdfToFile(const QString& target, const QVector<int>& pages, bool withReferences = false);
    /// Submits an asynchronous background PDF export and returns its job id
    /// immediately; the output file is finalized when the worker reports
    /// completion via pdfExportFinished. Returns nullopt when no source path
    /// exists, the transport is busy, or the submit failed.
    std::optional<quint64> startPdfExport(const QString& target, const QVector<int>& pages);
    Model::SignResponse signToFile(Model::SignRequest request, const QString& password, const QString& target);
    std::optional<Model::FormUpdateResponse> updateForm(const Model::FormUpdateRequest& request);
    std::optional<Model::FormUpdateResponse> resetForm(const Model::FormResetRequest& request);
    bool settings(const Model::DocumentSettings& settings);

signals:
    void processExited(int);
    void ocrDone(quint64, int);
    void pageLinksReady(quint64, std::vector<Model::PageLinks>, bool, QString);
    void pdfExportFinished(quint64 jobId, bool success, QString error);

private:
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
    void completePdfExport(bool success, QString error);

    /// Flushes the staged temporary file and atomically moves it onto target.
    /// The auto-remove flag is left enabled on failure so the staged data is
    /// cleaned up. When syncToDisk is set the data is fsynced before the move,
    /// which signing requires. Returns false and fills error on failure.
    bool finalizeTempFile(QTemporaryFile& file, const QString& target, bool syncToDisk, QString* error);

    // File-producing requests use a temporary file and rename it only after
    // the worker has completed, so a failed or interrupted export cannot
    // leave a partial destination file.
    template <class Payload> bool writeFile(Payload payload, const QString& target)
    {
        QFileInfo info(target);
        QTemporaryFile file(info.absolutePath() + QStringLiteral("/.mupdf-worker-XXXXXX"));
        if (!file.open()) {
            MU_LOG(warning,
                   "Mu::Plugin",
                   "could not create temporary file for " + target.toStdString() + ": "
                       + file.errorString().toStdString());
            return false;
        }
        const auto transfer = m_nextTransfer++;
        std::string e;
        if (!m_fd.send(transfer, file.handle(), &e)) {
            MU_LOG(warning, "Mu::Plugin", "could not send output FD for " + target.toStdString() + ": " + e);
            return false;
        }
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
        if (!finalizeTempFile(file, target, /*syncToDisk=*/false, &error)) {
            MU_LOG(warning,
                   "Mu::Plugin",
                   "could not finalize output for " + target.toStdString() + ": " + error.toStdString());
            return false;
        }
        return true;
    }

    bool sendOcrInput(QFile& input, std::uint64_t& transfer);
    Model::OpenStatus openFile(const QString& path,
                               const QString& password,
                               QList<Model::PageInfo>* pages,
                               Model::DocumentType type,
                               bool useEpubAcceleratorCache);
    // A request may receive asynchronous notifications while its response is
    // pending; call() serializes that exchange and handles both message kinds.
    std::optional<Model::ResponseMessage> requestOcr(int page, const QString& language, int dpi);
    std::optional<Model::ResponseMessage> call(Model::RequestPayload payload);
    void processIncomingNotifications();
    bool handleNotification(const Model::NotificationMessage& notification, std::string* error = nullptr);
    static void cleanupFrameLease(void* data);
    void releaseFrameSlot(quint64 session, quint64 slotId, quint64 leaseId);
    static QString findBinary(const QString& hint);
    void finished(int code, QProcess::ExitStatus status);
    void abort();
    void cleanupSession();

    QProcess m_process;
    IPC::CtrlChannel m_ctrl;
    IPC::FdChannel m_fd;
    std::unique_ptr<QSocketNotifier> m_notifier;
    QString m_socketPath, m_fdSocketPath, m_tempPath, m_sourcePath;
    QString m_activeSignPassword;
    Model::DocumentSettings m_settings;
    std::optional<PendingExport> m_export;
    std::unique_ptr<QTimer> m_exportTimer;
    std::unordered_map<std::uint64_t, std::shared_ptr<FrameSlotMapping>> m_frameSlots;
    quint64 m_nextId = 1, m_nextTransfer = 1, m_linkGeneration = 0;
    quint64 m_frameSession = 0;
#ifdef MU_DEBUG_ENABLED
    std::optional<std::chrono::steady_clock::time_point> m_pageLinksStartedAt;
#endif
    bool m_intentionalStop = false;
    bool m_inFlight = false;
    bool m_useEpubCache = false;
};

} // namespace Mu::Plugin

#endif // MU_PLUGIN_WORKER_TRANSPORT_HPP
