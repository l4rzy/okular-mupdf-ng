// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_client.hpp"

#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

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
    m_lifecycle.timer.setSingleShot(true);
    connect(&m_lifecycle.timer, &QTimer::timeout, this, &WorkerClient::restartWorker);
    m_thread = new QThread();
    m_transport = new WorkerTransport();
    m_transport->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_transport, &QObject::deleteLater);
    connect(
        m_transport,
        &WorkerTransport::processExited,
        this,
        [this](int exitCode) {
            // The worker is gone; its cached handshake facts no longer apply.
            m_lifecycle.info = { };
            // A running document session ended with the process. Recovery is
            // only complete once the generator revalidates and commits it.
            m_lifecycle.setState(State::Recovering);
            Q_EMIT workerDied(exitCode);
            scheduleRestart();
        },
        Qt::QueuedConnection);
    connect(m_transport, &WorkerTransport::ocrDone, this, &WorkerClient::ocrDone, Qt::QueuedConnection);
    connect(m_transport, &WorkerTransport::pageLinksReady, this, &WorkerClient::pageLinksReady, Qt::QueuedConnection);
    connect(
        m_transport, &WorkerTransport::pdfExportFinished, this, &WorkerClient::pdfExportFinished, Qt::QueuedConnection);
    m_thread->start();
}

WorkerClient::~WorkerClient()
{
    // Stop on the transport thread before joining it; deleting the thread
    // first could leave queued socket/process work accessing freed state.
    stop();
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
}

bool WorkerClient::start(const QString& binaryPath, const QStringList& tessDataDirectories)
{
    // A manual start resets automatic-recovery history and becomes the new
    // baseline for subsequent crash recovery.
    m_lifecycle.reset(binaryPath, tessDataDirectories);
    const bool started = startWorker();
    m_lifecycle.setState(started ? State::Ready : State::Stopped);
    return started;
}

bool WorkerClient::startWorker()
{
    // BlockingQueuedConnection is the synchronization boundary: the caller
    // sees the completed transport transition, while all transport state
    // remains thread-confined.
    bool result = false;
    const QString binaryPath = m_lifecycle.binary;
    const QStringList tessDataDirectories = m_lifecycle.tessDataDirectories;
    Model::PingResponse workerInfo;
    QMetaObject::invokeMethod(
        m_transport,
        [transport = m_transport, binaryPath, tessDataDirectories, &result, &workerInfo] {
            result = transport->start(binaryPath, tessDataDirectories, &workerInfo);
        },
        Qt::BlockingQueuedConnection);
    // The blocking call provides the happens-before edge for the cached info.
    m_lifecycle.info = result ? workerInfo : Model::PingResponse { };
    return result;
}

void WorkerClient::stop()
{
    // Mark the stop before invoking transport cleanup so an intentional exit
    // is not reported as a worker crash and restarted.
    m_lifecycle.beginStop();
    m_lifecycle.timer.stop();
    if (m_transport)
        QMetaObject::invokeMethod(m_transport, [t = m_transport] { t->stop(); }, Qt::BlockingQueuedConnection);
    m_lifecycle.info = { };
    m_lifecycle.setState(State::Stopped);
}

bool WorkerClient::isConnected() const
{
    return sync([&](WorkerTransport* transport) { return transport->isConnected(); });
}

std::optional<quint64> WorkerClient::workerMemoryBytes() const
{
    return sync([&](WorkerTransport* transport) { return transport->workerMemoryBytes(); });
}

OpenStatus WorkerClient::open(const QString& p, const QString& w, QList<PageInfo>& pages, DocumentType type)
{
    return sync([&](WorkerTransport* transport) { return transport->open(p, w, &pages, type); }, OpenStatus::Failed);
}

OpenStatus WorkerClient::openData(const QByteArray& d, const QString& p, QList<PageInfo>& pages, DocumentType type)
{
    return sync([&](WorkerTransport* transport) { return transport->openData(d, p, &pages, type); },
                OpenStatus::Failed);
}

bool WorkerClient::close()
{
    return sync([&](WorkerTransport* transport) { return transport->close(); });
}

QImage WorkerClient::render(int p, int w, int h, const QRect& t, const std::function<bool()>& shouldAbort)
{
    if (!shouldAbort)
        return sync([&](WorkerTransport* transport) { return transport->render(p, w, h, t); });
    if (shouldAbort() || !m_transport)
        return { };

    struct RenderState {
        std::mutex mutex;
        std::condition_variable completed;
        bool cancelled = false;
        bool done = false;
        QImage image;
        int cancellationFd = -1;
        void* cookie = MAP_FAILED;

        ~RenderState()
        {
            if (cookie != MAP_FAILED)
                ::munmap(cookie, IPC::RenderCookieBytes);
            if (cancellationFd >= 0)
                ::close(cancellationFd);
        }
    };

    const auto state = std::make_shared<RenderState>();
    state->cancellationFd = ::memfd_create("mupdf-render-cancel", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (state->cancellationFd >= 0 && ::ftruncate(state->cancellationFd, IPC::RenderCookieBytes) == 0
        && ::fcntl(state->cancellationFd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0) {
        state->cookie =
            ::mmap(nullptr, IPC::RenderCookieBytes, PROT_READ | PROT_WRITE, MAP_SHARED, state->cancellationFd, 0);
    }
    if (state->cookie == MAP_FAILED) {
        if (state->cancellationFd >= 0)
            ::close(state->cancellationFd);
        state->cancellationFd = -1;
        MU_LOG(warning, "Mu::Plugin", "render cancellation cookie unavailable; active render will finish normally");
    }
    // Only owned inputs cross the thread boundary. The abort predicate may
    // reference a PixmapRequest that is destroyed as soon as this call returns.
    if (!QMetaObject::invokeMethod(
            m_transport,
            [transport = m_transport, state, p, w, h, t] {
                {
                    const std::lock_guard lock(state->mutex);
                    if (state->cancelled)
                        return;
                }
                // Once dispatched, finish the exchange, including receiving
                // frame descriptors. Discarded images use normal lease cleanup.
                QImage image = transport->render(p, w, h, t, state->cancellationFd);
                {
                    const std::lock_guard lock(state->mutex);
                    if (!state->cancelled)
                        state->image = std::move(image);
                    state->done = true;
                }
                state->completed.notify_one();
            },
            Qt::QueuedConnection))
        return { };

    std::unique_lock lock(state->mutex);
    for (;;) {
        lock.unlock();
        const bool cancelled = shouldAbort();
        lock.lock();
        if (cancelled) {
            state->cancelled = true;
            // MuPDF explicitly permits an unsynchronized, one-way abort update.
            // This mapping belongs only to this job and outlives the active RPC.
            if (state->cookie != MAP_FAILED)
                *static_cast<volatile std::int32_t*>(state->cookie) = 1;
            return { };
        }
        if (state->done)
            return std::move(state->image);
        // Completion wakes immediately; poll only the caller-owned abort flag.
        state->completed.wait_for(lock, std::chrono::milliseconds(10));
    }
}

std::vector<TextBox> WorkerClient::getTextBoxesForPage(int p, qreal x, qreal y, bool skipAnnots, bool* success) const
{
    if (success)
        *success = false;
    return sync(
        [&](WorkerTransport* transport) { return transport->getTextBoxesForPage(p, x, y, skipAnnots, success); });
}

std::optional<quint64> WorkerClient::startOcrPage(int p, const QString& l, int d) const
{
    return sync([&](WorkerTransport* transport) { return transport->startOcrPage(p, l, d); });
}

OcrResult WorkerClient::ocrResult(quint64 id) const
{
    return sync([&](WorkerTransport* transport) { return transport->ocrResult(id); });
}

bool WorkerClient::cancelOcrJobs() const
{
    return sync([&](WorkerTransport* transport) { return transport->cancelOcrJobs(); });
}

std::vector<Font> WorkerClient::fonts(int p) const
{
    return sync([&](WorkerTransport* transport) { return transport->fonts(p); });
}

std::vector<EmbeddedFile> WorkerClient::embeddedFiles() const
{
    return sync([&](WorkerTransport* transport) { return transport->embeddedFiles(); });
}

std::vector<OutlineNode> WorkerClient::synopsis(bool allowHeuristic) const
{
    return sync([&](WorkerTransport* transport) { return transport->synopsis(allowHeuristic); });
}

DocumentMetadata WorkerClient::getDocumentInfo(const QStringList& k) const
{
    return sync([&](WorkerTransport* transport) { return transport->getDocumentInfo(k); });
}

std::optional<AnnotationHandle> WorkerClient::addAnnotation(int p, const Annotation& a) const
{
    return sync([&](WorkerTransport* transport) { return transport->addAnnotation(p, a); });
}

bool WorkerClient::modifyAnnotation(int p, const QString& h, const Annotation& a, bool c) const
{
    return sync([&](WorkerTransport* transport) { return transport->modifyAnnotation(p, h, a, c); });
}

bool WorkerClient::removeAnnotation(int p, const QString& h) const
{
    return sync([&](WorkerTransport* transport) { return transport->removeAnnotation(p, h); });
}

std::optional<Model::FormUpdateResponse> WorkerClient::updateForm(const Model::FormUpdateRequest& request) const
{
    return sync([&](WorkerTransport* transport) { return transport->updateForm(request); });
}

std::optional<Model::FormUpdateResponse> WorkerClient::resetForm(const Model::FormResetRequest& request) const
{
    return sync([&](WorkerTransport* transport) { return transport->resetForm(request); });
}

std::optional<Model::FormUpdateResponse>
WorkerClient::clickFormButton(const Model::FormButtonClickRequest& request) const
{
    return sync([&](WorkerTransport* transport) { return transport->clickFormButton(request); });
}

bool WorkerClient::saveToFile(const QString& t)
{
    return sync([&](WorkerTransport* transport) { return transport->saveToFile(t); });
}

bool WorkerClient::flattenPdfToFile(const QString& target, const QVector<int>& pages)
{
    return sync([&](WorkerTransport* transport) { return transport->flattenPdfToFile(target, pages); });
}

bool WorkerClient::savePdfToFile(const QString& t, const QVector<int>& pages, bool withReferences)
{
    return sync([&](WorkerTransport* transport) { return transport->savePdfToFile(t, pages, withReferences); });
}

std::optional<quint64> WorkerClient::startPdfExport(const QString& t, const QVector<int>& pages) const
{
    return sync([&](WorkerTransport* transport) { return transport->startPdfExport(t, pages); });
}

SignResponse WorkerClient::sign(const SignRequest& r, const QString& password, const QString& t)
{
    return sync([&](WorkerTransport* transport) { return transport->signToFile(r, password, t); },
                SignResponse { SigningResult::GenericError, "worker is unavailable" });
}

std::optional<Model::LayersResponse> WorkerClient::layers() const
{
    return sync([&](WorkerTransport* transport) { return transport->layers(); });
}

std::optional<Model::LayersResponse> WorkerClient::setLayer(const Model::SetLayerRequest& request) const
{
    return sync([&](WorkerTransport* transport) { return transport->setLayer(request); });
}

bool WorkerClient::setSettings(const DocumentSettings& settings)
{
    return sync([&](WorkerTransport* transport) { return transport->settings(settings); });
}

SandboxStatus WorkerClient::sandboxStatus() const
{
    return m_lifecycle.info.sandbox;
}

std::string WorkerClient::engineVersion() const
{
    return m_lifecycle.info.engineVersion;
}

void WorkerClient::LifeCycle::prune() noexcept
{
    const auto cutoff = std::chrono::steady_clock::now() - Window;
    while (!restartTimes.empty() && restartTimes.front() <= cutoff)
        restartTimes.pop_front();
}

void WorkerClient::LifeCycle::reset(const QString& binaryPath, const QStringList& directories)
{
    timer.stop();
    binary = binaryPath;
    tessDataDirectories = directories;
    info = { };
    stopping = false;
    restartsDisabled = false;
    attempts = 0;
    restartTimes.clear();
    sequence = 0;
}

bool WorkerClient::LifeCycle::budgetExhausted()
{
    prune();
    return attempts >= MaxAttempts || restartTimes.size() >= MaxRestartsPerWindow;
}

int WorkerClient::LifeCycle::nextRestartDelayMs() noexcept
{
    static constexpr int DelaysMs[] = { 250, 500, 1000 };
    return DelaysMs[attempts++];
}

void WorkerClient::LifeCycle::recordRestart()
{
    prune();
    restartTimes.push_back(std::chrono::steady_clock::now());
    ++sequence;
    attempts = 0;
}

void WorkerClient::scheduleRestart()
{
    // A worker that repeatedly dies must not create an endless restart storm.
    if (!m_lifecycle.canScheduleRestart() || m_lifecycle.timer.isActive())
        return;
    if (m_lifecycle.budgetExhausted()) {
        m_lifecycle.disableRestarts();
        m_lifecycle.timer.stop();
        MU_LOG(critical,
               "Mu::Plugin",
               "worker restart limit reached; automatic recovery disabled recentRestarts="
                   + std::to_string(m_lifecycle.recentRestarts())
                   + " windowSeconds=" + std::to_string(LifeCycle::Window.count()));
        m_lifecycle.setState(State::Failed);
        Q_EMIT workerUnavailable();
        return;
    }
    m_lifecycle.timer.start(m_lifecycle.nextRestartDelayMs());
}

void WorkerClient::restartWorker()
{
    // A successful restart only restores the process; the generator must
    // reopen its document and commit the session before operations resume.
    if (!m_lifecycle.canScheduleRestart() || isConnected())
        return;
    if (startWorker()) {
        m_lifecycle.recordRestart();
        m_lifecycle.setState(State::Recovering);
        MU_LOG(warning,
               "Mu::Plugin",
               "worker revived restart=" + std::to_string(m_lifecycle.sequence)
                   + " recentRestarts=" + std::to_string(m_lifecycle.recentRestarts())
                   + " windowSeconds=" + std::to_string(LifeCycle::Window.count()));
        Q_EMIT workerRestarted();
        return;
    }
    scheduleRestart();
}

void WorkerClient::commitSessionReady()
{
    m_lifecycle.setState(State::Ready);
}

void WorkerClient::commitSessionFailed()
{
    m_lifecycle.setState(State::Failed);
}

} // namespace Mu::Plugin
