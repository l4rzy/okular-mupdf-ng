// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/epub/export_jobs.hpp"

#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "engine/epub/document.hpp"
#include "shared/logging.hpp"

namespace Mu::Worker::Engine {

// =============================================================================
// Submission & Background Execution
// =============================================================================

ExportJobs::ExportJobs()
    : ExportJobs(::Mu::Worker::Sys::createEventFd().value_or(::Mu::Worker::Sys::FileDescriptor { }))
{
}

ExportJobs::ExportJobs(::Mu::Worker::Sys::FileDescriptor event)
{
    m_state->event = std::move(event);
    if (m_state->event.get() < 0)
        MU_LOG(warning, "Mu::Worker::Export", "eventfd creation failed; PDF export is disabled for this worker");
}

int ExportJobs::eventFd() const noexcept
{
    std::lock_guard lock(m_state->mutex);
    return m_state->event.get();
}

std::optional<std::uint64_t> ExportJobs::submit(int inputFd,
                                                int outputFd,
                                                const ::Mu::Model::DocumentSettings& settings,
                                                std::vector<std::int32_t> pages)
{
    // Both descriptors are consumed on every path below, including rejection.
    Sys::FileDescriptor ownedInput(inputFd);
    Sys::FileDescriptor ownedOutput(outputFd);

    std::uint64_t id = 0;
    {
        std::lock_guard lock(m_state->mutex);
        // Without a completion eventfd the notification could never be pumped
        // to the event loop, so accepting the job would strand it.
        if (m_state->active || m_state->event.get() < 0)
            return std::nullopt;
        id = m_state->nextId++;
        m_state->active = true;
    }

    auto state = m_state;
    std::thread([state,
                 id,
                 settings,
                 pages = std::move(pages),
                 inputFd = ownedInput.release(),
                 outputFd = ownedOutput.release()] {
        std::string error;
        bool success = false;
        {
            // Private document instance: the session engine is single-threaded
            // and must never be touched from this thread.
            EpubDocument document(static_cast<std::size_t>(settings.memoryCacheBytes));
            document.setSettings(settings);
            if (document.openFdWithAccelerator(inputFd, "export.epub", { }, &error)) {
                // savePdfFdWithReferences adopts and closes the output
                // descriptor on every exit path, so ownership simply moves to
                // the engine.
                success = document.savePdfFdWithReferences(outputFd, pages, &error);
            } else {
                // Opening consumed the input fd; the output fd is still ours.
                ::close(outputFd);
                error = error.empty() ? "could not open document for PDF export" : error;
            }
        }

        if (!success)
            MU_LOG(warning, "Mu::Worker::Export", "background PDF export failed: " + error);

        {
            std::lock_guard lock(state->mutex);
            state->active = false;
            state->notifications.push_back({ id, success, std::move(error) });
        }
        if (state->event.get() >= 0)
            (void)::eventfd_write(state->event.get(), 1);
    }).detach();

    return id;
}

// =============================================================================
// Notification Draining
// =============================================================================

std::vector<ExportJobs::Notification> ExportJobs::drainNotifications()
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

} // namespace Mu::Worker::Engine
