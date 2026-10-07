// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_OCR_JOBS_HPP
#define MU_WORKER_ENGINE_OCR_JOBS_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "engine/ocr/ocr.hpp"
#include "shared/model/types.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

enum class JobStatus : std::uint8_t { Queued, Running, Cancelled };

/**
 * Asynchronous OCR job pool manager.
 *
 * Concurrency & Signaling Model:
 * 1. Spawns isolated background worker threads to perform Tesseract OCR page text
 *    extraction without blocking the main event loop.
 * 2. Each job executes in an isolated private Fitz context.
 * 3. Notifies the main worker loop by writing an 8-byte counter to an eventfd descriptor.
 * 4. Job entries move from Queued to Running or Cancelled under one mutex. A
 *    completion still registered in activeJobs is stored as a result and then
 *    announced through the eventfd notification queue, including watchdog
 *    cancellation. Host cancellation removes entries to discard their results,
 *    while executingJobs retains capacity until the runner and watchdog finish.
 */
class OcrJobs {
public:
    /// Notification payload emitted when an asynchronous OCR job finishes.
    struct Notification {
        std::uint64_t id = 0;
        int page = -1;
    };

    explicit OcrJobs(std::size_t limit = 8);
    /// Allows deterministic execution and a shorter watchdog in runtime tests.
    OcrJobs(std::size_t limit, decltype(&runOcr) runner, int watchdogTicks);
    ~OcrJobs();

    OcrJobs(const OcrJobs&) = delete;
    OcrJobs& operator=(const OcrJobs&) = delete;
    OcrJobs(OcrJobs&&) noexcept = delete;
    OcrJobs& operator=(OcrJobs&&) noexcept = delete;

    /// Returns the Linux eventfd descriptor notified on job completion.
    [[nodiscard]] int eventFd() const noexcept;

    /// Submits a background OCR job for a specific document page.
    /// Consumes `inputFd` on every path: rejection and pre-start cancellation
    /// close it here; a started job transfers it to runOcr for closure.
    [[nodiscard]] std::optional<std::uint64_t> submit(int inputFd,
                                                      std::string password,
                                                      int page,
                                                      std::string language,
                                                      float dpi,
                                                      std::string tessDataDirectory = { });

    /// Drains all completed OCR job notifications.
    [[nodiscard]] std::vector<Notification> drainNotifications();

    /// Takes the finished OcrResult payload for a given job ID.
    [[nodiscard]] std::optional<::Mu::Model::OcrResult> take(std::uint64_t id);

    /// Cancels active OCR jobs and clears completed results and notifications.
    /// Removed entries prevent stale results from being published. Detached workers
    /// retain their execution capacity until the runner and watchdog finish.
    void cancelAll() noexcept;

private:
    struct JobEntry {
        std::uint64_t id = 0;
        int page = -1;
        JobStatus status = JobStatus::Queued;
        std::shared_ptr<CancellationCookie> cookie = std::make_shared<CancellationCookie>();
    };

    struct SharedState {
        std::mutex mutex;
        std::map<std::uint64_t, JobEntry> activeJobs;
        // Includes cancelled jobs whose detached threads have not finished OCR.
        std::size_t executingJobs = 0;
        std::map<std::uint64_t, ::Mu::Model::OcrResult> completedResults;
        std::deque<Notification> notifications;
        ::Mu::Worker::Sys::FileDescriptor event;
        std::uint64_t nextId = 1;
    };

    std::size_t m_limit;
    decltype(&runOcr) m_runner;
    int m_watchdogTicks;
    std::shared_ptr<SharedState> m_state = std::make_shared<SharedState>();
};

} // namespace Mu::Worker::Engine
#endif // MU_WORKER_ENGINE_OCR_JOBS_HPP
