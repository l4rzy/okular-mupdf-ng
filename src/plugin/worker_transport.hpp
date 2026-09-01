// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_WORKER_TRANSPORT_HPP
#define MUPDF_PLUGIN_WORKER_TRANSPORT_HPP

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
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

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
    Q_INVOKABLE bool
    start(const QString& hint, const QStringList& tessDataDirectories, Model::SandboxStatus* sandboxStatus);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void abort();
    Q_INVOKABLE bool isConnected() const;
    Q_INVOKABLE Model::OpenStatus open(const QString& path,
                                       const QString& password,
                                       QList<Model::PageInfo>* pages,
                                       Model::DocumentType type = Model::DocumentType::Pdf);
    Q_INVOKABLE Model::OpenStatus openData(const QByteArray& data,
                                           const QString& password,
                                           QList<Model::PageInfo>* pages,
                                           Model::DocumentType type = Model::DocumentType::Pdf);
    Q_INVOKABLE bool close();
    Q_INVOKABLE QImage render(int page, int width, int height, const QRect& rect);
    Q_INVOKABLE std::vector<Model::TextBox> getTextBoxesForPage(int page, qreal x, qreal y, bool skipAnnots = false);
    Q_INVOKABLE Model::OcrResult ocrPage(int page, const QString& language, int dpi, bool async);
    Q_INVOKABLE std::optional<quint64> startOcrPage(int page, const QString& language, int dpi);
    Q_INVOKABLE Model::OcrResult ocrResult(quint64 id);
    Q_INVOKABLE bool cancelOcrJobs();
    Q_INVOKABLE std::vector<Model::Font> fonts(int page);
    Q_INVOKABLE std::vector<Model::EmbeddedFile> embeddedFiles();
    Q_INVOKABLE std::vector<Model::OutlineNode> synopsis();
    Q_INVOKABLE Model::DocumentMetadata getDocumentInfo(const QStringList& keys);
    Q_INVOKABLE std::optional<Model::AnnotationHandle> addAnnotation(int page, const Model::Annotation& annotation);
    Q_INVOKABLE bool
    modifyAnnotation(int page, const QString& handle, const Model::Annotation& annotation, bool appearance);
    Q_INVOKABLE bool removeAnnotation(int page, const QString& handle);
    Q_INVOKABLE bool saveToFile(const QString& target);
    Q_INVOKABLE bool savePdfToFile(const QString& target, const QVector<int>& pages);
    Q_INVOKABLE Model::SignResponse
    signToFile(Model::SignRequest request, const QString& password, const QString& target);
    Q_INVOKABLE std::optional<Model::FormUpdateResponse> updateForm(const Model::FormUpdateRequest& request);
    Q_INVOKABLE std::optional<Model::FormUpdateResponse> resetForm(const Model::FormResetRequest& request);
    Q_INVOKABLE bool settings(const Model::DocumentSettings& settings);

signals:
    void workerDied(int);
    void ocrDone(quint64, int);
    void pageLinksReady(quint64, std::vector<Model::PageLinks>, bool, QString);

private:
    struct FrameSlotMapping;
    struct FrameLease;

    // File-producing requests use a temporary file and rename it only after
    // the worker has completed, so a failed or interrupted export cannot
    // leave a partial destination file.
    template <class Payload> bool writeFile(Payload payload, const QString& target)
    {
        QFileInfo info(target);
        QTemporaryFile file(info.absolutePath() + QStringLiteral("/.mupdf-worker-XXXXXX"));
        if (!file.open())
            return false;
        const auto transfer = m_nextTransfer++;
        std::string e;
        if (!m_fd.send(transfer, file.handle(), &e))
            return false;
        payload.file.transferId = transfer;
        auto response = call(std::move(payload));
        if (!response || response->error || !file.flush())
            return false;
        file.setAutoRemove(false);
        if (::rename(QFile::encodeName(file.fileName()).constData(), QFile::encodeName(target).constData())) {
            file.setAutoRemove(true);
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
    std::optional<Model::ResponseMessage> requestOcr(int page, const QString& language, int dpi, bool async);
    std::optional<Model::ResponseMessage> call(Model::RequestPayload payload);
    void processIncomingNotifications();
    bool handleNotification(const Model::NotificationMessage& notification, std::string* error = nullptr);
    static void cleanupFrameLease(void* data);
    void releaseFrameSlot(quint64 session, quint64 slotId, quint64 leaseId);
    static QString findBinary(const QString& hint);
    void finished(int code, QProcess::ExitStatus status);
    void cleanupSession();

    QProcess m_process;
    IPC::CtrlChannel m_ctrl;
    IPC::FdChannel m_fd;
    std::unique_ptr<QSocketNotifier> m_notifier;
    QString m_socketPath, m_fdSocketPath, m_tempPath, m_sourcePath;
    QString m_activeSignPassword;
    Model::DocumentSettings m_settings;
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

#endif
