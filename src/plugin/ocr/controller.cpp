// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/ocr/controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include "plugin/ocr/constants.hpp"
#include "shared/logging.hpp"

namespace Mu::Plugin::OCR {

Controller::Controller(WorkerClient* backend, QObject* parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(Constant::DEBOUNCE_MS);
    connect(&m_debounce, &QTimer::timeout, this, &Controller::settle);
    connect(m_backend, &WorkerClient::ocrDone, this, &Controller::finish);
    connect(&m_cacheWatcher,
            &QFutureWatcher<Caching::OCR::CacheLoadResult>::finished,
            this,
            &Controller::cacheLoadFinished);
}

bool Controller::shouldTrigger(bool force, bool autoTrigger, unsigned threshold, std::size_t existingTextBoxCount)
{
    if (force)
        return true;
    return autoTrigger && existingTextBoxCount < threshold;
}

void Controller::observeVisiblePages(const QList<VisiblePage>& visiblePages, const Config& config)
{
    // Focus hysteresis keeps OCR anchored while the user scrolls: the page
    // that dominates the viewport wins, with a bias toward the previous focus.
    const int focusPage = dominantPage(visiblePages, m_lastFocusPage);
    if (focusPage < 0)
        return;
    m_lastFocusPage = focusPage;
    observe(focusPage, config);
}

void Controller::observe(int page, Config config)
{
    // Observation may originate from generator callbacks; queue it so all
    // scheduler state changes happen on the controller's QObject thread. A
    // reset invalidates observations that were already queued for an old
    // document.
    const std::uint64_t generation = m_generation;
    QMetaObject::invokeMethod(
        this,
        [this, page, config = std::move(config), generation] {
            if (generation != m_generation)
                return;
            const bool configChanged = (m_config != config);
            m_config = config;
            if (configChanged) {
                // A new document/configuration invalidates both queued work
                // and results from an older asynchronous cache operation.
                m_cacheWatcher.cancel();
                m_pendingCache.reset();
                m_queue.clear();
                if (m_activeJob) {
                    m_backend->cancelOcrJobs();
                    m_activeJob.reset();
                }
                m_nativeTextBoxCounts.clear();
            }
            if (m_scheduler.observe(page, config.pageCount) || configChanged)
                m_debounce.start();
        },
        Qt::QueuedConnection);
}

std::optional<QVector<Caching::OCR::CacheItem>> Controller::takeReady(int page)
{
    QMutexLocker locker(&m_readyMutex);
    const auto ready = m_readyResults.find(page);
    if (ready == m_readyResults.end())
        return std::nullopt;
    const auto boxes = std::move(ready.value());
    m_readyResults.erase(ready);
    return boxes;
}

void Controller::reset()
{
    // Reset is the document-lifecycle boundary. The future may finish later,
    // but its pending request is discarded before any result is consumed.
    ++m_generation;
    m_cacheWatcher.cancel();
    m_debounce.stop();
    m_scheduler.reset();
    m_queue.clear();
    m_activeJob.reset();
    m_pendingCache.reset();
    m_nativeTextBoxCounts.clear();
    m_lastFocusPage = -1;
    {
        QMutexLocker locker(&m_readyMutex);
        m_readyResults.clear();
    }
}

bool Controller::shouldRun(int page)
{
    if (!m_backend->isConnected())
        return false;
    if (m_config.force)
        return true;
    if (!m_config.autoTrigger)
        return false;

    if (const auto it = m_nativeTextBoxCounts.constFind(page); it != m_nativeTextBoxCounts.cend())
        return shouldTrigger(m_config.force, m_config.autoTrigger, m_config.triggerThreshold, *it);

    const auto existingBoxes = m_backend->getTextBoxesForPage(page, m_config.dpiX, m_config.dpiY, /*skipAnnots=*/true);
    const auto count = existingBoxes.size();
    m_nativeTextBoxCounts.insert(page, count);
    return shouldTrigger(m_config.force, m_config.autoTrigger, m_config.triggerThreshold, count);
}

void Controller::settle()
{
    // Replace stale prefetch work with the current focus window after the
    // debounce period has confirmed that scrolling has settled.
    const QList<int> pages = m_scheduler.settle();
    if (pages.isEmpty())
        return;

    m_queue.clear();
    for (int page : pages) {
        if (shouldRun(page))
            m_queue.append(page);
    }
    cancelObsoleteWork();
    startNext();
}

void Controller::cancelObsoleteWork()
{
    if (m_activeJob && m_scheduler.shouldCancelRunning(m_activeJob->page)) {
        m_backend->cancelOcrJobs();
        m_activeJob.reset();
    }
    if (m_activeJob)
        m_queue.removeAll(m_activeJob->page);
    if (m_pendingCache && !m_queue.contains(m_pendingCache->page)) {
        // QtConcurrent cannot reliably stop filesystem work, so cancellation
        // here means suppressing delivery and any subsequent OCR dispatch.
        m_cacheWatcher.cancel();
        m_pendingCache.reset();
    }
}

void Controller::startNext()
{
    // At most one cache load or worker OCR job is active. This keeps ordering
    // deterministic and prevents background prefetch from starving focus.
    if (m_activeJob || m_pendingCache || m_cacheWatcher.isRunning() || m_queue.isEmpty())
        return;
    const auto key = Caching::OCR::Cache::normalizeKey(m_config.documentHash, m_config.language, m_config.dpi);
    if (!key)
        return;
    const int page = m_queue.takeFirst();
    m_pendingCache = PendingCache { page, *key };
    m_cacheWatcher.setFuture(QtConcurrent::run([page, key = *key] { return Caching::OCR::Cache::load(key, page); }));
}

void Controller::cacheLoadFinished()
{
    // The watcher can finish after reset/configuration invalidated its request;
    // the presence of m_pendingCache decides whether it is still safe to use
    // the result.
    if (!m_pendingCache) {
        startNext();
        return;
    }

    const auto pending = std::move(*m_pendingCache);
    m_pendingCache.reset();
    const auto cached = m_cacheWatcher.result();
    if (cached.present) {
        {
            QMutexLocker locker(&m_readyMutex);
            m_readyResults.insert(pending.page, cached.items);
        }
        Q_EMIT completed(pending.page, cached.items, CompletionSource::CacheLoaded);
        startNext();
        return;
    }

    const auto job = m_backend->startOcrPage(pending.page, pending.key.language, pending.key.dpi);
    if (!job) {
        MU_LOG(warning, "Mu::Plugin::OCR", "failed to dispatch OCR job; waiting for a later observation");
        return;
    }
    m_activeJob = ActiveJob { *job, pending.page, pending.key };
    Q_EMIT started(pending.page);
}

void Controller::finish(quint64 jobId, int page)
{
    // Ignore late worker notifications for canceled or superseded jobs.
    if (!m_activeJob || jobId != m_activeJob->jobId || page != m_activeJob->page)
        return;
    const ActiveJob active = std::move(*m_activeJob);
    m_activeJob.reset();
    const auto result = m_backend->ocrResult(jobId);
    if (result.status == Model::OcrStatus::Success) {
        QVector<Caching::OCR::CacheItem> boxes = Caching::OCR::Cache::convertToCacheItems(result.boxes);
        Caching::OCR::Cache::save(active.cacheKey, page, boxes);
        {
            QMutexLocker locker(&m_readyMutex);
            m_readyResults.insert(page, boxes);
        }
        Q_EMIT completed(page, std::move(boxes), CompletionSource::OcrCompleted);
    } else {
        MU_LOG(warning,
               "Mu::Plugin::OCR",
               "OCR job failed for page " + std::to_string(page)
                   + " status=" + std::to_string(static_cast<int>(result.status)));
    }
    startNext();
}

} // namespace Mu::Plugin::OCR
