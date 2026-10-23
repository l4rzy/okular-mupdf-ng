// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/ocr/controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include "plugin/ocr/constants.hpp"
#include "shared/logging.hpp"

namespace Mu::Plugin::OCR {

// =============================================================================
// OCR Controller Setup & Page Observation
// =============================================================================

Controller::Controller(WorkerClient* backend, QObject* parent)
    : QObject(parent)
    , m_backend(backend)
    , m_retry(Constant::MAX_ATTEMPTS)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(Constant::DEBOUNCE_MS);
    connect(&m_debounce, &QTimer::timeout, this, &Controller::settle);

    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(Constant::RETRY_MS);
    connect(&m_retryTimer, &QTimer::timeout, this, &Controller::startNext);

    connect(m_backend, &WorkerClient::ocrDone, this, &Controller::finish);
    connect(&m_cacheWatcher,
            &QFutureWatcher<Caching::OCR::CacheLoadResult>::finished,
            this,
            &Controller::cacheLoadFinished);
}

void Controller::observeVisiblePages(const QList<VisiblePage>& visiblePages,
                                     const Config& config,
                                     std::optional<NativeTextObservation> nativeText)
{
    // Focus hysteresis keeps OCR anchored while the user scrolls: the page
    // that dominates the viewport wins, with a bias toward the previous focus.
    const int focusPage = dominantPage(visiblePages, m_lastFocusPage);
    if (focusPage < 0)
        return;
    m_lastFocusPage = focusPage;
    observe(focusPage, config, std::move(nativeText));
}

void Controller::observe(int page, Config config, std::optional<NativeTextObservation> nativeText)
{
    // Observation may originate from generator callbacks; queue it so all
    // scheduler state changes happen on the controller's QObject thread. A
    // reset invalidates observations that were already queued for an old
    // document.
    const std::uint64_t generation = m_generation;
    QMetaObject::invokeMethod(
        this,
        [this, page, config = std::move(config), nativeText = std::move(nativeText), generation] {
            if (generation != m_generation)
                return;
            const bool configChanged = (m_config != config);
            m_config = config;
            if (configChanged)
                invalidate();
            if (nativeText && nativeText->page >= 0 && nativeText->page < config.pageCount)
                m_nativeTextBoxCounts.insert(nativeText->page, nativeText->boxCount);
            // Apply the configured settle delay before (re)arming, only when it
            // changed: setInterval() restarts a running timer, and untouched
            // observations must not extend an in-flight debounce.
            if (m_debounce.interval() != config.debounceMs)
                m_debounce.setInterval(config.debounceMs);
            if (m_focus.noteFocus(page, config.pageCount) || configChanged)
                m_debounce.start();
        },
        Qt::QueuedConnection);
}

// =============================================================================
// Ready Results & Invalidation
// =============================================================================

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
    m_debounce.stop();
    m_retryTimer.stop();
    m_focus.reset();
    m_lastFocusPage = -1;
    invalidate();
}

void Controller::invalidate()
{
    // A new document/configuration invalidates queued work, retry state, and
    // results from an older asynchronous cache operation.
    m_cacheWatcher.cancel();
    m_pendingCache.reset();
    m_queue.clear();
    m_retry.clear();
    if (m_activeJob) {
        m_backend->cancelOcrJobs();
        m_activeJob.reset();
    }
    m_nativeTextBoxCounts.clear();
    QMutexLocker locker(&m_readyMutex);
    m_readyResults.clear();
}

// =============================================================================
// Result Delivery & Scheduling
// =============================================================================

void Controller::deliver(int page, QVector<Caching::OCR::CacheItem> boxes, CompletionSource source)
{
    {
        QMutexLocker locker(&m_readyMutex);
        m_readyResults.insert(page, boxes);
    }
    Q_EMIT completed(page, std::move(boxes), source);
}

bool Controller::shouldSchedulePage(int page)
{
    // Recovering workers have no document yet; only a committed session may
    // receive OCR work.
    if (!m_backend->operational())
        return false;
    if (m_config.force)
        return true;
    if (!m_config.autoTrigger)
        return false;

    auto count = m_nativeTextBoxCounts.find(page);
    if (count == m_nativeTextBoxCounts.end()) {
        // Only focus justifies a native-text RPC; neighbours are cache hints.
        if (page != m_focus.focus())
            return false;
        const auto boxes = m_backend->getTextBoxesForPage(page, m_config.dpiX, m_config.dpiY, /*skipAnnots=*/true);
        count = m_nativeTextBoxCounts.insert(page, boxes.size());
    }
    return count.value() < m_config.triggerThreshold;
}

void Controller::settle()
{
    // Rebuild the prefetch window after the debounce has confirmed that
    // scrolling settled. Worker OCR is limited to the focus page; neighbours
    // are queued so their caches can be probed cheaply.
    const QList<int> candidates = m_focus.prefetchWindow();
    if (candidates.isEmpty())
        return;

    QList<int> wanted;
    for (int page : candidates) {
        if (shouldSchedulePage(page))
            wanted.append(page);
    }
    m_queue.refresh(m_focus.focus(), wanted, Constant::STALE_RADIUS, Constant::MAX_QUEUED_PAGES);
    startNext();
}

void Controller::startNext()
{
    // Serialize cache probes and recognition. Focus changes update the queue
    // without cancelling the active job; reset/configuration changes cancel it.
    if (m_activeJob || m_pendingCache || m_cacheWatcher.isRunning())
        return;

    const auto page = m_queue.takeNext(m_focus.focus(), Constant::STALE_RADIUS);
    if (!page)
        return;

    const auto key = Caching::OCR::Cache::normalizeKey(m_config.documentHash, m_config.language, m_config.dpi);
    if (!key) {
        m_queue.pushFront(*page, m_focus.focus(), Constant::STALE_RADIUS);
        return;
    }
    m_pendingCache = PendingCache { *page, *key };
    m_cacheWatcher.setFuture(
        QtConcurrent::run([page = *page, key = *key] { return Caching::OCR::Cache::load(key, page); }));
}

// =============================================================================
// Cache & Worker Completion Handling
// =============================================================================

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
        m_retry.clear(pending.page);
        deliver(pending.page, cached.items, CompletionSource::CacheLoaded);
    } else if (pending.page == m_focus.focus()) {
        // Neighbours only probe the cache. A miss starts recognition only if
        // this page is still focused when the asynchronous lookup completes.
        if (const auto job = m_backend->startOcrPage(pending.page, pending.key.language, pending.key.dpi)) {
            m_activeJob = ActiveJob { *job, pending.page, pending.key };
            Q_EMIT started(pending.page);
        } else {
            handleFailure(pending.page);
        }
        return;
    }
    startNext();
}

void Controller::finish(quint64 jobId, int page)
{
    // Ignore late worker notifications for canceled or superseded jobs.
    if (!m_activeJob || jobId != m_activeJob->jobId || page != m_activeJob->page)
        return;
    const ActiveJob active = std::move(*m_activeJob);
    m_activeJob.reset();
    const auto result = m_backend->ocrResult(jobId);
    if (result.status != Model::OcrStatus::Success) {
        handleFailure(page);
        return;
    }

    m_retry.clear(page);
    QVector<Caching::OCR::CacheItem> boxes = Caching::OCR::Cache::convertToCacheItems(result.boxes);
    Caching::OCR::Cache::save(active.cacheKey, page, boxes);
    deliver(page, std::move(boxes), CompletionSource::OcrCompleted);
    startNext();
}

void Controller::handleFailure(int page)
{
    if (m_retry.onFailure(page)) {
        m_queue.pushFront(page, m_focus.focus(), Constant::STALE_RADIUS);
        m_retryTimer.start();
        return;
    }

    m_retry.clear(page);
    MU_LOG(warning, "Mu::Plugin::OCR", "OCR failed repeatedly for page " + std::to_string(page));
    Q_EMIT failed(page);
}

} // namespace Mu::Plugin::OCR
