// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ocr/jobs.hpp"

#include <chrono>
#include <exception>

#include "engine/ocr/ocr.hpp"
#include "shared/logging.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

// =============================================================================
// Construction, Destruction & Event Descriptor
// =============================================================================

OcrJobs::OcrJobs(std::size_t limit)
    : m_limit(limit)
{
    // Create Linux eventfd for cross-thread non-blocking poll loop wakeups
    auto event = ::Mu::Worker::Sys::createEventFd();
    if (event) {
        m_state->event = std::move(*event);
    } else {
        MU_LOG(warning, "Mu::Worker::Ocr", "eventfd creation failed; asynchronous OCR is disabled for this worker");
    }
}

OcrJobs::~OcrJobs()
{
    cancelAll();
    std::lock_guard lock(m_state->mutex);
    m_state->event.reset();
}

int OcrJobs::eventFd() const noexcept
{
    std::lock_guard lock(m_state->mutex);
    return m_state->event.get();
}

// =============================================================================
// Background Job Submission & Watchdog Lifecycle
// =============================================================================

// Spawns an isolated background worker thread per job. Entries move from
// Queued -> Running or Cancelled under SharedState::mutex; non-cancelled
// completions move into completedResults before their eventfd notification.
std::optional<std::uint64_t>
OcrJobs::submit(int inputFd, std::string password, int page, std::string language, float dpi)
{
    // Owns the descriptor on every return/throw path; ownership transfers to the
    // worker thread only once it has started.
    Sys::FileDescriptor ownedInput(inputFd);
    if (!ownedInput)
        return std::nullopt;

    std::uint64_t id = 0;
    std::shared_ptr<CancellationCookie> cookie;
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->event.get() < 0 || m_state->activeJobs.size() + m_state->completedResults.size() >= m_limit) {
            return std::nullopt;
        }

        id = m_state->nextId++;
        cookie = std::make_shared<CancellationCookie>();
        m_state->activeJobs.emplace(id, JobEntry { id, page, JobStatus::Queued, cookie });
    }

    auto state = m_state;
    std::thread worker;
    try {
        worker = std::thread([state,
                              cookie,
                              id,
                              page,
                              inputFd = ownedInput.get(),
                              password = std::move(password),
                              language = std::move(language),
                              dpi]() mutable {
            // Transition to Running if not cancelled before thread startup
            {
                std::lock_guard lock(state->mutex);
                auto it = state->activeJobs.find(id);
                if (it == state->activeJobs.end() || it->second.status == JobStatus::Cancelled
                    || cookie->isCancelled()) {
                    if (it != state->activeJobs.end())
                        state->activeJobs.erase(it);
                    ::close(inputFd);
                    return;
                }
                it->second.status = JobStatus::Running;
            }

            // 60-second watchdog timer: bounds OCR work even when a malformed
            // page makes MuPDF/Tesseract spend unusually long in a device
            // callback.
            std::jthread deadline([cookie](std::stop_token watchdogStop) {
                for (int tick = 0; tick < 600 && !watchdogStop.stop_requested(); ++tick)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!watchdogStop.stop_requested())
                    cookie->cancel();
            });

            // Run isolated synchronous OCR page recognition (adopts and closes
            // inputFd)
            auto result = runOcr(inputFd, password, page, language, dpi, cookie.get());
            deadline.request_stop();

            // Atomically remove the active entry, store a non-cancelled result,
            // and queue its eventfd notification under the same mutex.
            {
                std::lock_guard lock(state->mutex);
                auto it = state->activeJobs.find(id);
                if (it != state->activeJobs.end()) {
                    const bool cancelled = it->second.status == JobStatus::Cancelled || cookie->isCancelled()
                        || result.status == Model::OcrStatus::Cancelled;
                    const int completedPage = it->second.page;
                    state->activeJobs.erase(it);
                    if (!cancelled) {
                        state->completedResults.emplace(id, std::move(result));
                        state->notifications.push_back({ id, completedPage });
                        if (state->event.get() >= 0) {
                            (void)::eventfd_write(state->event.get(), 1);
                        }
                    }
                }
            }
        });
    } catch (const std::exception& e) {
        // Thread creation failed before the job ran: release the reserved slot
        // and let the RAII owner close the input descriptor.
        MU_LOG(warning, "Mu::Worker::Ocr", std::string("could not start OCR thread: ") + e.what());
        std::lock_guard lock(m_state->mutex);
        m_state->activeJobs.erase(id);
        return std::nullopt;
    }
    worker.detach();

    // The running thread now owns the input descriptor.
    (void)ownedInput.release();
    return id;
}

// =============================================================================
// Notification Draining & Result Extraction
// =============================================================================

std::vector<OcrJobs::Notification> OcrJobs::drainNotifications()
{
    std::lock_guard lock(m_state->mutex);
    // eventfd coalesces wakeups, so consume its counter before draining every
    // queued completion notification under the same synchronization boundary.
    eventfd_t value = 0;
    while (m_state->event.get() >= 0 && ::eventfd_read(m_state->event.get(), &value) == 0) { }

    std::vector<Notification> result(m_state->notifications.begin(), m_state->notifications.end());
    m_state->notifications.clear();
    return result;
}

std::optional<::Mu::Model::OcrResult> OcrJobs::take(std::uint64_t id)
{
    std::lock_guard lock(m_state->mutex);
    const auto it = m_state->completedResults.find(id);
    if (it == m_state->completedResults.end())
        return std::nullopt;

    auto value = std::move(it->second);
    m_state->completedResults.erase(it);
    return value;
}

// =============================================================================
// Cooperative Job Cancellation
// =============================================================================

void OcrJobs::cancelAll() noexcept
{
    std::lock_guard lock(m_state->mutex);
    for (auto& [id, entry] : m_state->activeJobs) {
        (void)id;
        entry.status = JobStatus::Cancelled;
        if (entry.cookie)
            entry.cookie->cancel();
    }
    // Remove entries immediately so a reopened document does not inherit the
    // previous document's capacity usage. Workers still hold the shared state
    // and will close their input FD without publishing a stale result.
    m_state->activeJobs.clear();
    // Results belong to the document that produced them and cannot survive
    // cancellation or document replacement.
    m_state->completedResults.clear();
    m_state->notifications.clear();
}

} // namespace Mu::Worker::Engine
