// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_client.hpp"

#include "shared/model/validation.hpp"

#include <chrono>
#include <deque>

#include "plugin/worker_transport.hpp"
#include "shared/logging.hpp"

namespace Mu::Plugin {

using namespace ::Mu::Model;

WorkerClient::WorkerClient(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<std::vector<Model::PageLinks>>();
    // Keep the transport event loop independent from the generator event
    // loop. This prevents socket waits and worker notifications from blocking
    // UI-facing plugin calls.
    m_restartTimer.setSingleShot(true);
    connect(&m_restartTimer, &QTimer::timeout, this, &WorkerClient::restartWorker);
    m_thread = new QThread();
    m_transport = new WorkerTransport();
    m_transport->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_transport, &QObject::deleteLater);
    connect(
        m_transport,
        &WorkerTransport::workerDied,
        this,
        [this](int exitCode) {
            // The worker is gone; its sandbox status no longer applies.
            m_sandboxStatus = { };
            Q_EMIT workerDied(exitCode);
            scheduleRestart();
        },
        Qt::QueuedConnection);
    connect(m_transport, &WorkerTransport::ocrDone, this, &WorkerClient::ocrDone, Qt::QueuedConnection);
    connect(m_transport, &WorkerTransport::pageLinksReady, this, &WorkerClient::pageLinksReady, Qt::QueuedConnection);
    m_thread->start();
}

WorkerClient::~WorkerClient()
{
    // Stop on the transport thread before joining it; deleting the thread
    // first could leave queued socket/process work accessing freed state.
    m_restartTimer.stop();
    if (m_transport)
        QMetaObject::invokeMethod(m_transport, "stop", Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
}

bool WorkerClient::start(const QString& binaryPath, const QStringList& tessDataDirectories)
{
    // A manual start resets automatic-recovery history and becomes the new
    // baseline for subsequent crash recovery.
    m_stopping = false;
    m_restartTimer.stop();
    m_restartAttempts = 0;
    m_restartTimes.clear();
    m_restartSequence = 0;
    m_restartDisabled = false;
    m_workerBinary = binaryPath;
    m_tessDataDirectories = tessDataDirectories;
    return startWorker(binaryPath);
}

bool WorkerClient::startWorker(const QString& binaryPath)
{
    // BlockingQueuedConnection is the synchronization boundary: the caller
    // sees the completed transport transition, while all transport state
    // remains thread-confined.
    bool result = false;
    const QStringList tessDataDirectories = m_tessDataDirectories;
    Model::SandboxStatus status;
    QMetaObject::invokeMethod(
        m_transport,
        [transport = m_transport, binaryPath, tessDataDirectories, &result, &status] {
            result = transport->start(binaryPath, tessDataDirectories, &status);
        },
        Qt::BlockingQueuedConnection);
    // The blocking call provides the happens-before edge for the status.
    m_sandboxStatus = result ? status : Model::SandboxStatus { };
    return result;
}

void WorkerClient::stop()
{
    // Mark the stop before invoking transport cleanup so an intentional exit
    // is not reported as a worker crash and restarted.
    m_stopping = true;
    m_restartTimer.stop();
    if (m_transport)
        QMetaObject::invokeMethod(m_transport, "stop", Qt::BlockingQueuedConnection);
    m_sandboxStatus = { };
}

bool WorkerClient::isConnected() const
{
    bool result = false;
    if (m_transport)
        QMetaObject::invokeMethod(
            m_transport, [&] { result = m_transport->isConnected(); }, Qt::BlockingQueuedConnection);
    return result;
}

OpenStatus WorkerClient::open(const QString& p, const QString& w, QList<PageInfo>& pages, DocumentType type)
{
    OpenStatus result = OpenStatus::Failed;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->open(p, w, &pages, type); }, Qt::BlockingQueuedConnection);
    return result;
}

OpenStatus WorkerClient::openData(const QByteArray& d, const QString& p, QList<PageInfo>& pages, DocumentType type)
{
    OpenStatus result = OpenStatus::Failed;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->openData(d, p, &pages, type); }, Qt::BlockingQueuedConnection);
    return result;
}

bool WorkerClient::close()
{
    bool result = false;
    QMetaObject::invokeMethod(m_transport, [&] { result = m_transport->close(); }, Qt::BlockingQueuedConnection);
    return result;
}

QImage WorkerClient::render(int p, int w, int h, const QRect& t)
{
    QImage result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->render(p, w, h, t); }, Qt::BlockingQueuedConnection);
    return result;
}

std::vector<TextBox> WorkerClient::getTextBoxesForPage(int p, qreal x, qreal y, bool skipAnnots) const
{
    std::vector<TextBox> result;
    QMetaObject::invokeMethod(
        m_transport,
        [&] { result = m_transport->getTextBoxesForPage(p, x, y, skipAnnots); },
        Qt::BlockingQueuedConnection);
    return result;
}

OcrResult WorkerClient::ocrPage(int p, const QString& l, int d, bool a) const
{
    OcrResult result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->ocrPage(p, l, d, a); }, Qt::BlockingQueuedConnection);
    return result;
}

std::optional<quint64> WorkerClient::startOcrPage(int p, const QString& l, int d) const
{
    std::optional<quint64> result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->startOcrPage(p, l, d); }, Qt::BlockingQueuedConnection);
    return result;
}

OcrResult WorkerClient::ocrResult(quint64 id) const
{
    OcrResult result;
    QMetaObject::invokeMethod(m_transport, [&] { result = m_transport->ocrResult(id); }, Qt::BlockingQueuedConnection);
    return result;
}

bool WorkerClient::cancelOcrJobs() const
{
    bool result = false;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->cancelOcrJobs(); }, Qt::BlockingQueuedConnection);
    return result;
}

std::vector<Font> WorkerClient::fonts(int p) const
{
    std::vector<Font> result;
    QMetaObject::invokeMethod(m_transport, [&] { result = m_transport->fonts(p); }, Qt::BlockingQueuedConnection);
    return result;
}

std::vector<EmbeddedFile> WorkerClient::embeddedFiles() const
{
    std::vector<EmbeddedFile> result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->embeddedFiles(); }, Qt::BlockingQueuedConnection);
    return result;
}

std::vector<OutlineNode> WorkerClient::synopsis() const
{
    std::vector<OutlineNode> result;
    QMetaObject::invokeMethod(m_transport, [&] { result = m_transport->synopsis(); }, Qt::BlockingQueuedConnection);
    return result;
}

DocumentMetadata WorkerClient::getDocumentInfo(const QStringList& k) const
{
    DocumentMetadata result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->getDocumentInfo(k); }, Qt::BlockingQueuedConnection);
    return result;
}

std::optional<AnnotationHandle> WorkerClient::addAnnotation(int p, const Annotation& a) const
{
    std::optional<AnnotationHandle> result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->addAnnotation(p, a); }, Qt::BlockingQueuedConnection);
    return result;
}

bool WorkerClient::modifyAnnotation(int p, const QString& h, const Annotation& a, bool c) const
{
    bool result = false;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->modifyAnnotation(p, h, a, c); }, Qt::BlockingQueuedConnection);
    return result;
}

bool WorkerClient::removeAnnotation(int p, const QString& h) const
{
    bool result = false;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->removeAnnotation(p, h); }, Qt::BlockingQueuedConnection);
    return result;
}

std::optional<Model::FormUpdateResponse> WorkerClient::updateForm(const Model::FormUpdateRequest& request) const
{
    if (!Model::isValidFormUpdateRequest(request))
        return std::nullopt;

    std::optional<Model::FormUpdateResponse> result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->updateForm(request); }, Qt::BlockingQueuedConnection);
    return result;
}

std::optional<Model::FormUpdateResponse> WorkerClient::resetForm(const Model::FormResetRequest& request) const
{
    if (!Model::isValidFormResetRequest(request))
        return std::nullopt;

    std::optional<Model::FormUpdateResponse> result;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->resetForm(request); }, Qt::BlockingQueuedConnection);
    return result;
}

bool WorkerClient::saveToFile(const QString& t)
{
    bool result = false;
    QMetaObject::invokeMethod(m_transport, [&] { result = m_transport->saveToFile(t); }, Qt::BlockingQueuedConnection);
    return result;
}

bool WorkerClient::printPdfToFile(const QString& t, const QVector<int>& pages)
{
    bool result = false;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->savePdfToFile(t, pages); }, Qt::BlockingQueuedConnection);
    return result;
}

SignResponse WorkerClient::sign(const SignRequest& r, const QString& password, const QString& t)
{
    SignResponse result { SigningResult::GenericError, "worker is unavailable" };
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->signToFile(r, password, t); }, Qt::BlockingQueuedConnection);
    return result;
}

bool WorkerClient::setSettings(const DocumentSettings& settings)
{
    bool result = false;
    QMetaObject::invokeMethod(
        m_transport, [&] { result = m_transport->settings(settings); }, Qt::BlockingQueuedConnection);
    return result;
}

SandboxStatus WorkerClient::sandboxStatus() const
{
    return m_sandboxStatus;
}

void WorkerClient::pruneRestartHistory()
{
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    while (!m_restartTimes.empty() && m_restartTimes.front() <= cutoff)
        m_restartTimes.pop_front();
}

void WorkerClient::scheduleRestart()
{
    // Recovery is deliberately bounded in both delay and frequency. A worker
    // that repeatedly dies must not create an endless restart storm.
    if (m_stopping || m_restartDisabled || m_restartTimer.isActive() || m_restartAttempts >= 3)
        return;
    pruneRestartHistory();
    if (m_restartTimes.size() >= 2) {
        m_restartDisabled = true;
        m_restartTimer.stop();
        MU_LOG(critical,
               "Mu::Plugin",
               "worker restart limit reached; automatic recovery disabled recentRestarts="
                   + std::to_string(m_restartTimes.size()) + " windowSeconds=5");
        Q_EMIT workerUnavailable();
        return;
    }
    static constexpr int delaysMs[] = { 250, 500, 1000 };
    m_restartTimer.start(delaysMs[m_restartAttempts++]);
}

void WorkerClient::restartWorker()
{
    // A successful restart only restores the process; the generator must
    // reopen its document after receiving workerRestarted.
    if (m_stopping || m_restartDisabled || isConnected())
        return;
    if (startWorker(m_workerBinary)) {
        pruneRestartHistory();
        m_restartTimes.push_back(std::chrono::steady_clock::now());
        ++m_restartSequence;
        m_restartAttempts = 0;
        MU_LOG(warning,
               "Mu::Plugin",
               "worker revived restart=" + std::to_string(m_restartSequence)
                   + " recentRestarts=" + std::to_string(m_restartTimes.size()) + " windowSeconds=5");
        Q_EMIT workerRestarted();
        return;
    }
    scheduleRestart();
}

} // namespace Mu::Plugin
