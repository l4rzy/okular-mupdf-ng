// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/export/jobs.hpp"

#include <exception>
#include <limits>
#include <sys/eventfd.h>
#include <thread>
#include <utility>

#include "engine/document_factory.hpp"
#include "engine/export/formats.hpp"
#include "shared/logging.hpp"
#include "sys/operation_budget.hpp"

namespace Mu::Worker::Engine {

// =============================================================================
// Submission & Background Execution
// =============================================================================

ExportJobs::ExportJobs()
    : ExportJobs(::Mu::Worker::Sys::createEventFd().value_or(::Mu::Worker::Sys::FileDescriptor { }))
{
}

ExportJobs::ExportJobs(::Mu::Worker::Sys::FileDescriptor event)
    : ExportJobs(std::move(event), createDocument)
{
}

ExportJobs::ExportJobs(::Mu::Worker::Sys::FileDescriptor event, DocumentFactory factory)
    : m_factory(factory)
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

    // Prepare the failure message before starting the job so reporting an
    // allocation failure does not require another allocation.
    auto settingsCopy = settings;
    std::string exceptionError = "background PDF export failed with an exception";

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
                              factory = m_factory,
                              input = std::move(ownedInput),
                              output = std::move(ownedOutput),
                              exceptionError = std::move(exceptionError)]() mutable {
            const Sys::OperationBudget budget;
            std::string error;
            bool success = false;
            try {
                // The export owns a private document, independent of the session.
                const auto storeSize = static_cast<std::size_t>(settings.memoryCacheBytes);
                if (auto document = factory(type, storeSize)) {
                    document->setSettings(settings);
                    auto mime = Model::documentTypeToMime(type);
                    if (document->openFd(input.release(), std::move(mime), &error)) {
                        const DocumentBase::ExportProgressCallback progress = [state](int completed, int total) {
                            std::lock_guard lock(state->mutex);
                            state->progress->update(completed, total);
                            // The event loop samples page counts. Wake it when finalization starts.
                            if (completed == total)
                                (void)::eventfd_write(state->event.get(), 1);
                        };
                        success = document->exportFd(output.release(), options, true, &error, progress);
                    } else if (error.empty()) {
                        error = "could not open document for PDF export";
                    }
                } else {
                    error = "unsupported ebook type";
                }
                if (!success)
                    MU_LOG(warning, "Mu::Worker::Export", error);
            } catch (...) {
                error.swap(exceptionError);
            }
            input.reset();
            output.reset();

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
    } catch (const std::exception&) {
        // Startup failed before the job ran. RAII closes both descriptors.
        std::lock_guard lock(m_state->mutex);
        m_state->active = false;
        m_state->progress.reset();
        return std::nullopt;
    }
    worker.detach();
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
