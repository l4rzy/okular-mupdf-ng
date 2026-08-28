// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_ENGINE_OCR_JOBS_HPP
#define MUPDF_WORKER_ENGINE_OCR_JOBS_HPP

#include <atomic>
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

#include "engine/cancellation_cookie.hpp"
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
 *    non-cancelled completion is removed from activeJobs, stored as a result,
 *    and then announced through the eventfd notification queue.
 */
class OcrJobs {
public:
    /// Notification payload emitted when an asynchronous OCR job finishes.
    struct Notification {
        std::uint64_t id = 0;
        int page = -1;
    };

    explicit OcrJobs(std::size_t limit = 8);
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
    [[nodiscard]] std::optional<std::uint64_t>
    submit(int inputFd, std::string password, int page, std::string language, float dpi);

    /// Drains all completed OCR job notifications.
    [[nodiscard]] std::vector<Notification> drainNotifications();

    /// Takes the finished OcrResult payload for a given job ID.
    [[nodiscard]] std::optional<::Mu::Model::OcrResult> take(std::uint64_t id);

    /// Cancels active OCR jobs and clears completed results and notifications.
    /// Detached workers retain shared state only long enough to observe cancellation;
    /// removed entries prevent them from publishing results for a replaced document.
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
        std::map<std::uint64_t, ::Mu::Model::OcrResult> completedResults;
        std::deque<Notification> notifications;
        ::Mu::Worker::Sys::FileDescriptor event;
        std::uint64_t nextId = 1;
    };

    std::size_t m_limit;
    std::shared_ptr<SharedState> m_state = std::make_shared<SharedState>();
};

} // namespace Mu::Worker::Engine
#endif
