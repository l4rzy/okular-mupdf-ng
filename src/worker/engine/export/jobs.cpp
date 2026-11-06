// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/export/jobs.hpp"

#include <exception>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "engine/document_factory.hpp"
#include "engine/export/formats.hpp"
#include "shared/logging.hpp"
#include "sys/operation_budget.hpp"
#include <limits>

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
                                                ::Mu::Model::ExportOptions options,
                                                ::Mu::Model::DocumentType type)
{
    // Both descriptors are consumed on every path below, including rejection.
    Sys::FileDescriptor ownedInput(inputFd);
    Sys::FileDescriptor ownedOutput(outputFd);
    if (!Model::findExportProfile(outputFormatsFor(type), options, true, std::numeric_limits<std::int32_t>::max()))
        return std::nullopt;

    // Copy settings before reserving the slot. Moving the copy into the closure
    // keeps its construction noexcept, so thread startup is the only fallible
    // step after the reservation.
    ::Mu::Model::DocumentSettings settingsCopy = settings;

    std::uint64_t id = 0;
    {
        std::lock_guard lock(m_state->mutex);
        // Without a completion eventfd the notification could never be pumped
        // to the event loop, so accepting the job would strand it.
        if (m_state->active || m_state->completion || m_state->event.get() < 0)
            return std::nullopt;
        id = m_state->nextId++;
        m_state->active = true;
        m_state->progress.emplace(id);
    }

    auto state = m_state;
    std::thread worker;
    try {
        worker = std::thread([state,
                              id,
                              type,
                              settings = std::move(settingsCopy),
                              options = std::move(options),
                              inputFd = ownedInput.get(),
                              outputFd = ownedOutput.get()] {
            const Sys::OperationBudget budget;
            std::string error;
            bool success = false;
            {
                // Private document instance: the session engine is
                // single-threaded and must never be touched from this thread.
                const auto storeSize = static_cast<std::size_t>(settings.memoryCacheBytes);
                auto document = createDocument(type, storeSize);
                if (document)
                    document->setSettings(settings);
                if (!document) {
                    ::close(inputFd);
                    ::close(outputFd);
                    error = "unsupported ebook type";
                } else if (document->openFd(inputFd, Model::documentTypeToMime(type), &error)) {
                    // exportFd adopts and closes the output
                    // descriptor on every exit path, so ownership simply moves
                    // to the engine.
                    success = document->exportFd(outputFd, options, true, &error, [state](int completed, int total) {
                        std::lock_guard lock(state->mutex);
                        state->progress->update(completed, total);
                        // Page counts are sampled by the event loop. Wake it
                        // immediately only when the finalization phase starts.
                        if (completed == total)
                            (void)::eventfd_write(state->event.get(), 1);
                    });
                } else {
                    // Opening consumed the input fd; the output fd is still
                    // ours.
                    ::close(outputFd);
                    error = error.empty() ? "could not open document for PDF export" : error;
                }
            }

            if (!success)
                MU_LOG(warning, "Mu::Worker::Export", "background PDF export failed: " + error);

            {
                std::lock_guard lock(state->mutex);
                state->active = false;
                state->completion = Model::ExportProgressNotification {
                    id, 0, 0, success ? Model::ExportPhase::Succeeded : Model::ExportPhase::Failed, std::move(error)
                };
            }
            if (state->event.get() >= 0)
                (void)::eventfd_write(state->event.get(), 1);
        });
    } catch (const std::exception& e) {
        // Thread creation failed before the job ran: no thread owns the
        // descriptors and no completion will clear the reservation, so roll it
        // back and let the RAII owners close the descriptors on return.
        MU_LOG(warning, "Mu::Worker::Export", std::string("could not start export thread: ") + e.what());
        std::lock_guard lock(m_state->mutex);
        m_state->active = false;
        m_state->progress.reset();
        return std::nullopt;
    }
    worker.detach();

    // The running thread now owns both descriptors.
    (void)ownedInput.release();
    (void)ownedOutput.release();
    return id;
}

// =============================================================================
// Notification Draining
// =============================================================================

std::vector<Model::ExportProgressNotification> ExportJobs::drainNotifications()
{
    std::lock_guard lock(m_state->mutex);
    // A single lock orders finalization before completion, including when the
    // export finishes between eventfd wakeup and sampling.
    eventfd_t value = 0;
    while (m_state->event.get() >= 0 && ::eventfd_read(m_state->event.get(), &value) == 0) { }

    std::vector<Model::ExportProgressNotification> result;
    if (m_state->progress) {
        if (auto progress = m_state->progress->take())
            result.push_back(std::move(*progress));
    }
    if (m_state->completion) {
        result.push_back(std::move(*m_state->completion));
        m_state->completion.reset();
        m_state->progress.reset();
    }
    return result;
}

bool ExportJobs::active() const
{
    std::lock_guard lock(m_state->mutex);
    return m_state->active;
}

} // namespace Mu::Worker::Engine
