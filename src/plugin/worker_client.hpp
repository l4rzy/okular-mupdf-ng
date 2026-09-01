// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_WORKER_CLIENT_HPP
#define MUPDF_PLUGIN_WORKER_CLIENT_HPP

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <chrono>
#include <deque>

#include "shared/model/types.hpp"

namespace Mu::Plugin {

class WorkerTransport;

/**
 * Thread-marshalled facade over the worker transport.
 *
 * The generator calls this object from its own thread; each operation is
 * marshalled to WorkerTransport, which owns the sockets and process. The
 * blocking calls are deliberate: callers receive a complete result while
 * transport state remains confined to one thread.
 */
class WorkerClient final : public QObject {
    Q_OBJECT

public:
    using PageInfo = Model::PageInfo;
    explicit WorkerClient(QObject* parent = nullptr);
    ~WorkerClient() override;
    bool start(const QString& binaryPath, const QStringList& tessDataDirectories = { });
    void stop();
    bool isConnected() const;
    Model::OpenStatus open(const QString& path,
                           const QString& password,
                           QList<PageInfo>& pages,
                           Model::DocumentType type = Model::DocumentType::Pdf);
    Model::OpenStatus openData(const QByteArray& data,
                               const QString& password,
                               QList<PageInfo>& pages,
                               Model::DocumentType type = Model::DocumentType::Pdf);
    bool close();
    QImage render(int page, int width, int height, const QRect& tile = { });
    std::vector<Model::TextBox> getTextBoxesForPage(int page, qreal dpiX, qreal dpiY, bool skipAnnots = false) const;
    Model::DocumentMetadata getDocumentInfo(const QStringList& keys = { }) const;
    Model::OcrResult ocrPage(int page, const QString& language, int dpi, bool asynchronous) const;
    std::optional<quint64> startOcrPage(int page, const QString& language, int dpi) const;
    Model::OcrResult ocrResult(quint64 jobId) const;
    bool cancelOcrJobs() const;
    std::vector<Model::Font> fonts(int page) const;
    std::vector<Model::EmbeddedFile> embeddedFiles() const;
    std::vector<Model::OutlineNode> synopsis() const;
    bool setSettings(const Model::DocumentSettings& settings);
    bool saveToFile(const QString& target);
    bool printPdfToFile(const QString& target, const QVector<int>& pages);
    Model::SignResponse sign(const Model::SignRequest& request, const QString& password, const QString& target);
    std::optional<Model::AnnotationHandle> addAnnotation(int page, const Model::Annotation& annotation) const;
    bool
    modifyAnnotation(int page, const QString& id, const Model::Annotation& annotation, bool appearanceChanged) const;
    bool removeAnnotation(int page, const QString& id) const;
    std::optional<Model::FormUpdateResponse> updateForm(const Model::FormUpdateRequest& request) const;
    std::optional<Model::FormUpdateResponse> resetForm(const Model::FormResetRequest& request) const;
    [[nodiscard]] Model::SandboxStatus sandboxStatus() const;
signals:
    void workerDied(int exitCode);
    /// A fresh worker is available. It deliberately has no document open.
    void workerRestarted();
    /// Automatic recovery is disabled after repeated worker failures.
    void workerUnavailable();
    void ocrDone(quint64 jobId, int page);
    void
    pageLinksReady(quint64 generation, std::vector<Model::PageLinks> pages, bool resourceLimited, const QString& error);

private:
    // Restart state is owned by the client thread; transport failures are
    // converted into bounded, delayed recovery attempts here.
    bool startWorker(const QString& binaryPath);
    void pruneRestartHistory();
    void scheduleRestart();
    void restartWorker();

    QThread* m_thread = nullptr;
    WorkerTransport* m_transport = nullptr;
    QTimer m_restartTimer;
    QString m_workerBinary;
    QStringList m_tessDataDirectories;
    int m_restartAttempts = 0;
    std::deque<std::chrono::steady_clock::time_point> m_restartTimes;
    quint64 m_restartSequence = 0;
    bool m_restartDisabled = false;
    bool m_stopping = false;
    // Cached copy of the worker's sandbox status, refreshed only at
    // synchronized points (start, stop, worker death) so generator-thread
    // reads need no cross-thread round trip.
    Model::SandboxStatus m_sandboxStatus;
};

} // namespace Mu::Plugin

#endif
