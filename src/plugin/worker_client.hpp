// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_WORKER_CLIENT_HPP
#define MU_PLUGIN_WORKER_CLIENT_HPP

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <chrono>
#include <deque>
#include <type_traits>
#include <utility>

#include "shared/model/form_backend.hpp"
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
class WorkerClient final : public QObject, public Model::FormBackend {
    Q_OBJECT

public:
    /// Worker process/session lifecycle. The process can be running while the
    /// generator has not yet confirmed its document session (Recovering); user
    /// operations must be gated on State::Ready.
    enum class State { Stopped, Ready, Recovering, Failed };

    using PageInfo = Model::PageInfo;
    explicit WorkerClient(QObject* parent = nullptr);
    ~WorkerClient() override;
    bool start(const QString& binaryPath, const QStringList& tessDataDirectories = { });
    void stop();
    bool isConnected() const;

    [[nodiscard]] State state() const noexcept { return m_lifecycle.current(); }

    [[nodiscard]] bool operational() const noexcept { return state() == State::Ready; }

    /// Marks recovery complete after the generator revalidated its document.
    void commitSessionReady();
    /// Marks recovery unrecoverable; no automatic restart will make it Ready.
    void commitSessionFailed();
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
    bool savePdfToFile(const QString& target, const QVector<int>& pages, bool withReferences = false);
    /// Submits an asynchronous background PDF export; returns the job id
    /// immediately. Completion arrives via the pdfExportFinished signal.
    std::optional<quint64> startPdfExport(const QString& target, const QVector<int>& pages) const;
    Model::SignResponse sign(const Model::SignRequest& request, const QString& password, const QString& target);
    std::optional<Model::AnnotationHandle> addAnnotation(int page, const Model::Annotation& annotation) const;
    bool
    modifyAnnotation(int page, const QString& id, const Model::Annotation& annotation, bool appearanceChanged) const;
    bool removeAnnotation(int page, const QString& id) const;
    std::optional<Model::FormUpdateResponse> updateForm(const Model::FormUpdateRequest& request) const override;
    std::optional<Model::FormUpdateResponse> resetForm(const Model::FormResetRequest& request) const override;
    [[nodiscard]] Model::SandboxStatus sandboxStatus() const;
    [[nodiscard]] std::string engineVersion() const;
signals:
    void workerDied(int exitCode);
    /// A fresh worker is available. It deliberately has no document open.
    void workerRestarted();
    /// Automatic recovery is disabled after repeated worker failures.
    void workerUnavailable();
    void ocrDone(quint64 jobId, int page);
    void
    pageLinksReady(quint64 generation, std::vector<Model::PageLinks> pages, bool resourceLimited, const QString& error);
    void pdfExportFinished(quint64 jobId, bool success, const QString& error);

private:
    // Everything tied to the worker process lifecycle: launch configuration,
    // restart-budget policy, cached handshake facts, the restart timer, and
    // the observable state used for operation gating.
    struct LifeCycle {
        // Launch configuration, reused for every restart.
        QString binary;
        QStringList tessDataDirectories;
        Model::PingResponse info;

        // Restart budget: at most MaxAttempts delayed retries, and no more
        // than MaxRestartsPerWindow successful restarts within Window.
        static constexpr int MaxAttempts = 3;
        static constexpr std::size_t MaxRestartsPerWindow = 2;
        static constexpr std::chrono::seconds Window { 5 };

        QTimer timer;
        bool stopping = false;
        bool restartsDisabled = false;
        int attempts = 0;
        std::deque<std::chrono::steady_clock::time_point> restartTimes;
        quint64 sequence = 0;

        // Generator render threads read this; the client thread writes it.
        std::atomic<State> state { State::Stopped };

        void reset(const QString& binaryPath, const QStringList& directories);

        void beginStop() noexcept { stopping = true; }

        [[nodiscard]] bool canScheduleRestart() const noexcept { return !stopping && !restartsDisabled; }

        [[nodiscard]] bool budgetExhausted();
        [[nodiscard]] int nextRestartDelayMs() noexcept;
        void recordRestart();

        void disableRestarts() noexcept { restartsDisabled = true; }

        [[nodiscard]] std::size_t recentRestarts() const noexcept { return restartTimes.size(); }

        [[nodiscard]] State current() const noexcept { return state.load(std::memory_order_acquire); }

        void setState(State value) noexcept { state.store(value, std::memory_order_release); }

    private:
        void prune() noexcept;
    };

    // Restart policy is owned by the client thread; transport failures are
    // converted into bounded, delayed recovery attempts here.
    bool startWorker();
    void scheduleRestart();
    void restartWorker();

    // Runs a transport call on the transport thread and returns its result,
    // seeded with a failure value so a failed dispatch cannot masquerade as
    // success. BlockingQueuedConnection is the synchronization boundary while
    // all transport state stays thread-confined.
    template <class Fn, class Result = std::invoke_result_t<Fn, WorkerTransport*>>
    Result sync(Fn&& fn, Result initial = { }) const
    {
        Result result = std::move(initial);
        if (!m_transport)
            return result;
        QMetaObject::invokeMethod(
            m_transport,
            [t = m_transport, fn = std::forward<Fn>(fn), &result] { result = fn(t); },
            Qt::BlockingQueuedConnection);
        return result;
    }

    QThread* m_thread = nullptr;
    WorkerTransport* m_transport = nullptr;
    LifeCycle m_lifecycle;
};

} // namespace Mu::Plugin

#endif // MU_PLUGIN_WORKER_CLIENT_HPP
