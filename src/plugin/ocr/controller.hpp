// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "plugin/caching/ocr_cache.hpp"
#include "plugin/ocr/config.hpp"
#include "plugin/ocr/scheduler.hpp"
#include "plugin/worker_client.hpp"

namespace Mu::Plugin::OCR {

class Controller final : public QObject {
    Q_OBJECT

public:
    // CompletionSource lets the UI distinguish actual worker recognition from
    // a result restored from the persistent OCR cache.
    enum class CompletionSource { CacheLoaded, OcrCompleted };

    explicit Controller(WorkerClient* backend, QObject* parent = nullptr);
    void observe(int page, Config config);
    void reset();

    static bool shouldTrigger(bool force, bool autoTrigger, unsigned threshold, std::size_t existingTextBoxCount);

signals:
    void started(int page);
    void completed(int page, QVector<Caching::OCR::CacheItem> boxes, CompletionSource source);

private:
    // Scheduling, cache loading, and worker calls all run on this QObject's
    // thread. The future only performs filesystem/decompression work off it.
    bool shouldRun(int page);
    void settle();
    void cancelObsoleteWork();
    void startNext();
    void cacheLoadFinished();
    void finish(quint64 jobId, int page);

    struct ActiveJob {
        quint64 jobId = 0;
        int page = -1;
        Caching::OCR::CacheKey cacheKey;
    };

    // Reset invalidates queued observations and all in-flight document work.
    struct PendingCache {
        int page = -1;
        Caching::OCR::CacheKey key;
    };

    WorkerClient* m_backend;
    QTimer m_debounce;
    QFutureWatcher<Caching::OCR::CacheLoadResult> m_cacheWatcher;
    Scheduler m_scheduler;
    Config m_config;
    QList<int> m_queue;
    std::optional<ActiveJob> m_activeJob;
    std::optional<PendingCache> m_pendingCache;
    // Native text counts are enough for OCR threshold decisions. The cache is
    // document-scoped because reset() is called whenever the worker document changes.
    QHash<int, std::size_t> m_nativeTextBoxCounts;
    std::uint64_t m_generation = 0;
};

} // namespace Mu::Plugin::OCR
