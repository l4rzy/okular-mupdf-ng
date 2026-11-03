// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_EPUB_EXPORT_JOBS_HPP
#define MU_WORKER_ENGINE_EPUB_EXPORT_JOBS_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "engine/epub/export_progress.hpp"
#include "shared/model/types.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

/**
 * Asynchronous EPUB-to-PDF export job runner.
 *
 * Runs one export at a time in a detached thread that owns a private
 * EpubDocument opened from a transferred copy of the source file, so the
 * session document and the main event loop are never blocked or shared.
 * Page counts are sampled every two seconds from a latest-only mailbox.
 * Finalization and completion wake the eventfd-backed notification path.
 */
class ExportJobs {
public:
    ExportJobs();
    ~ExportJobs() = default;

    ExportJobs(const ExportJobs&) = delete;
    ExportJobs& operator=(const ExportJobs&) = delete;
    ExportJobs(ExportJobs&&) noexcept = delete;
    ExportJobs& operator=(ExportJobs&&) noexcept = delete;

    /// Test seam: constructs the job runner around a caller-provided eventfd
    /// descriptor, which may be invalid to exercise the disabled state.
    explicit ExportJobs(::Mu::Worker::Sys::FileDescriptor event);

    /// Returns the Linux eventfd descriptor notified on finalization and completion.
    [[nodiscard]] int eventFd() const noexcept;

    /// Submits a background export job. Consumes both descriptors on every
    /// path: rejection closes them here; a started job transfers ownership to
    /// the engine (DocumentBase::openFd/savePdfFdWithReferences close them on
    /// all paths). Returns nullopt for unsupported types or until the previous
    /// export's completion has been drained.
    [[nodiscard]] std::optional<std::uint64_t> submit(int inputFd,
                                                      int outputFd,
                                                      const ::Mu::Model::DocumentSettings& settings,
                                                      std::vector<std::int32_t> pages,
                                                      ::Mu::Model::DocumentType type = ::Mu::Model::DocumentType::Epub);

    /// Samples progress and drains completion atomically, in lifecycle order.
    [[nodiscard]] std::vector<Model::ExportProgressNotification> drainNotifications();

    [[nodiscard]] bool active() const;

private:
    struct SharedState {
        std::mutex mutex;
        std::optional<Model::ExportProgressNotification> completion;
        ::Mu::Worker::Sys::FileDescriptor event;
        std::uint64_t nextId = 1;
        bool active = false;
        std::optional<ExportProgress> progress;
    };

    std::shared_ptr<SharedState> m_state = std::make_shared<SharedState>();
};

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_EPUB_EXPORT_JOBS_HPP
