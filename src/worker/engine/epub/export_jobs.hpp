// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_EPUB_EXPORT_JOBS_HPP
#define MU_WORKER_ENGINE_EPUB_EXPORT_JOBS_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "shared/model/types.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

/**
 * Asynchronous EPUB-to-PDF export job runner.
 *
 * Runs one export at a time in a detached thread that owns a private
 * EpubDocument opened from a transferred copy of the source file, so the
 * session document and the main event loop are never blocked or shared.
 * Completion is announced through an eventfd-backed notification queue
 * following the OcrJobs signaling model.
 */
class ExportJobs {
public:
    /// Notification payload emitted when an asynchronous export job finishes.
    struct Notification {
        std::uint64_t id = 0;
        bool success = false;
        std::string error;
    };

    ExportJobs();
    ~ExportJobs() = default;

    ExportJobs(const ExportJobs&) = delete;
    ExportJobs& operator=(const ExportJobs&) = delete;
    ExportJobs(ExportJobs&&) noexcept = delete;
    ExportJobs& operator=(ExportJobs&&) noexcept = delete;

    /// Returns the Linux eventfd descriptor notified on job completion.
    [[nodiscard]] int eventFd() const noexcept;

    /// Submits a background export job. Consumes both descriptors on every
    /// path: rejection closes them here; a started job transfers ownership to
    /// the engine (DocumentBase::openFd/savePdfFdWithReferences close them on
    /// all paths). Returns nullopt while another export is running.
    [[nodiscard]] std::optional<std::uint64_t>
    submit(int inputFd, int outputFd, const ::Mu::Model::DocumentSettings& settings, std::vector<std::int32_t> pages);

    /// Drains all completed export job notifications.
    [[nodiscard]] std::vector<Notification> drainNotifications();

private:
    struct SharedState {
        std::mutex mutex;
        std::deque<Notification> notifications;
        ::Mu::Worker::Sys::FileDescriptor event;
        std::uint64_t nextId = 1;
        bool active = false;
    };

    std::shared_ptr<SharedState> m_state = std::make_shared<SharedState>();
};

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_EPUB_EXPORT_JOBS_HPP
