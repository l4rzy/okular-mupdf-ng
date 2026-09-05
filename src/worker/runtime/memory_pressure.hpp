// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_RUNTIME_MEMORY_PRESSURE_HPP
#define MU_WORKER_RUNTIME_MEMORY_PRESSURE_HPP

#include <cstdint>

namespace Mu::Worker::Runtime::MemoryPressure {

// Idle-only shrink thresholds. Scroll/zoom renders only accumulate counters;
// the worker event loop trims after the user pauses, allowing MuPDF to evict
// eligible cache entries between user-visible bursts.
inline constexpr long long IdleMinMs = 2000;
inline constexpr long long SinceTrimMinMs = 2000;
inline constexpr std::uint64_t MinRendersSinceTrim = 20;
inline constexpr std::uint64_t MinBytesSinceTrim = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t LargeFrameBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t LargeRendersSinceTrim = 4;

/// Pure idle-shrink decision: true only after an idle pause with enough
/// accumulated render pressure. Large frames arm early so zoomed pages do
/// not wait for 20 small renders.
[[nodiscard]] inline bool shouldTrimForIdle(std::uint64_t rendersSinceTrim,
                                            std::uint64_t bytesSinceTrim,
                                            long long idleMs,
                                            long long sinceTrimMs) noexcept
{
    if (idleMs < IdleMinMs || sinceTrimMs < SinceTrimMinMs)
        return false;
    if (bytesSinceTrim >= MinBytesSinceTrim || rendersSinceTrim >= MinRendersSinceTrim)
        return true;
    return bytesSinceTrim >= LargeFrameBytes && rendersSinceTrim >= LargeRendersSinceTrim;
}

} // namespace Mu::Worker::Runtime::MemoryPressure

#endif // MU_WORKER_RUNTIME_MEMORY_PRESSURE_HPP
