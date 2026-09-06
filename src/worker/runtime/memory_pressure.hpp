// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_RUNTIME_MEMORY_PRESSURE_HPP
#define MU_WORKER_RUNTIME_MEMORY_PRESSURE_HPP

#include <cstdint>

#include "shared/model/types.hpp"

namespace Mu::Worker::Runtime::MemoryPressure {

/// Idle-trim thresholds for one aggressiveness preset. Scroll/zoom renders
/// only accumulate counters; the worker event loop trims after the user
/// pauses, allowing MuPDF to evict eligible cache entries between
/// user-visible bursts.
struct IdleTrimThresholds {
    long long idleMinMs = 2000;
    long long sinceTrimMinMs = 2000;
    std::uint64_t minRendersSinceTrim = 20;
    std::uint64_t minBytesSinceTrim = 128ULL * 1024ULL * 1024ULL;
    std::uint64_t largeFrameBytes = 32ULL * 1024ULL * 1024ULL;
    std::uint64_t largeRendersSinceTrim = 4;
    /// Target store size for fz_shrink_store, as a percentage of the current
    /// size (lower evicts more).
    unsigned int storePercent = 50;
};

/// Clamps a configured idle trim to the known range. Unknown levels
/// degrade to Balanced so a mismatched plugin can never disable trimming
/// silently or arm it on every idle poll.
[[nodiscard]] inline std::int32_t normalizeIdleTrim(std::int32_t idleTrim) noexcept
{
    switch (idleTrim) {
    case ::Mu::Model::IdleTrimLevel::Off:
    case ::Mu::Model::IdleTrimLevel::Conservative:
    case ::Mu::Model::IdleTrimLevel::Balanced:
    case ::Mu::Model::IdleTrimLevel::Aggressive:
        return idleTrim;
    default:
        return ::Mu::Model::IdleTrimLevel::Balanced;
    }
}

/// Resolves a normalized idle trim to its thresholds (see
/// normalizeIdleTrim for unknown-level handling).
[[nodiscard]] inline IdleTrimThresholds idleTrimThresholdsForLevel(std::int32_t idleTrim) noexcept
{
    switch (idleTrim) {
    case ::Mu::Model::IdleTrimLevel::Conservative:
        return { 4000, 4000, 40, 256ULL * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL, 8, 75 };
    case ::Mu::Model::IdleTrimLevel::Aggressive:
        return { 1000, 1000, 10, 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL, 2, 25 };
    case ::Mu::Model::IdleTrimLevel::Balanced:
    default:
        return { };
    }
}

/// Idle poll quantum for the event loop: wake up no less often than the
/// preset's idle gate so an armed trim is observed promptly. Off keeps a
/// plain keepalive quantum.
[[nodiscard]] inline long long idleTrimPollMsForLevel(std::int32_t idleTrim) noexcept
{
    if (idleTrim == ::Mu::Model::IdleTrimLevel::Off)
        return 2000;
    return idleTrimThresholdsForLevel(idleTrim).idleMinMs;
}

/// Pure idle-trim decision: true only after an idle pause with enough
/// accumulated render pressure. Large frames arm early so zoomed pages do
/// not wait for many small renders.
[[nodiscard]] inline bool shouldIdleTrim(std::uint64_t rendersSinceTrim,
                                         std::uint64_t bytesSinceTrim,
                                         long long idleMs,
                                         long long sinceTrimMs,
                                         IdleTrimThresholds thresholds = { }) noexcept
{
    if (idleMs < thresholds.idleMinMs || sinceTrimMs < thresholds.sinceTrimMinMs)
        return false;
    if (bytesSinceTrim >= thresholds.minBytesSinceTrim || rendersSinceTrim >= thresholds.minRendersSinceTrim)
        return true;
    return bytesSinceTrim >= thresholds.largeFrameBytes && rendersSinceTrim >= thresholds.largeRendersSinceTrim;
}

} // namespace Mu::Worker::Runtime::MemoryPressure

#endif // MU_WORKER_RUNTIME_MEMORY_PRESSURE_HPP
