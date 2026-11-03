// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_WORKER_ENGINE_EPUB_EXPORT_PROGRESS_HPP
#define MU_WORKER_ENGINE_EPUB_EXPORT_PROGRESS_HPP

#include <chrono>
#include <optional>
#include <utility>

#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

/// Latest-only mailbox, sampled every two seconds. Callers synchronize access.
class ExportProgress {
public:
    using Clock = std::chrono::steady_clock;

    explicit ExportProgress(std::uint64_t id, Clock::time_point now = Clock::now())
        : m_id(id)
        , m_nextProbe(now + std::chrono::seconds(2))
    {
    }

    void update(int completed, int total)
    {
        m_pending = Model::ExportProgressNotification { m_id,
                                                        completed,
                                                        total,
                                                        completed == total ? Model::ExportPhase::Finalizing
                                                                           : Model::ExportPhase::CreatingPages,
                                                        { } };
    }

    std::optional<Model::ExportProgressNotification> take(Clock::time_point now = Clock::now())
    {
        if (!m_pending || (m_pending->phase != Model::ExportPhase::Finalizing && now < m_nextProbe))
            return std::nullopt;
        m_nextProbe = now + std::chrono::seconds(2);
        return std::exchange(m_pending, std::nullopt);
    }

private:
    std::uint64_t m_id;
    Clock::time_point m_nextProbe;
    std::optional<Model::ExportProgressNotification> m_pending;
};

} // namespace Mu::Worker::Engine
#endif
