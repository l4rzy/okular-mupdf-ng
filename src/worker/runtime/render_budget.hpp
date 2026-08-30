// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_RUNTIME_RENDER_BUDGET_HPP
#define MUPDF_WORKER_RUNTIME_RENDER_BUDGET_HPP

#include <cmath>
#include <cstdint>
#include <utility>

#include "shared/model/types.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/transport/frame_buffer.hpp"

namespace Mu::Worker::Runtime {

/// Render request and frame layout fitted to one shared-memory segment.
struct FittedRenderRequest {
    Model::RenderRequest request;
    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;
    std::uint32_t frameStride = 0;
    std::uint64_t frameDataBytes = 0;
};

namespace Detail {

inline std::uint64_t rgbaDataBytes(int width, int height) noexcept
{
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4U;
}

inline int scaledDimension(int dimension, long double scale) noexcept
{
    const auto scaled = static_cast<int>(std::floor(static_cast<long double>(dimension) * scale));
    return scaled > 0 ? scaled : 1;
}

inline int scaledCoordinate(int coordinate, long double scale) noexcept
{
    return static_cast<int>(std::floor(static_cast<long double>(coordinate) * scale));
}

} // namespace Detail

/// Fits a validated page or tile render to the shared-memory frame budget.
///
/// The frame header consumes part of the segment limit. Oversized regions use
/// one uniform scale factor; tiles scale their virtual canvas and rectangle so
/// the returned image still represents the requested normalized page region.
[[nodiscard]] inline FittedRenderRequest fitRenderRequestToFrameBudget(const Model::RenderRequest& request) noexcept
{
    const auto* tile = request.tile ? &*request.tile : nullptr;
    const int sourceWidth = tile ? tile->width : request.width;
    const int sourceHeight = tile ? tile->height : request.height;
    constexpr std::uint64_t maxFrameDataBytes = Limit::MaxSharedFrameBytes - sizeof(IPC::FrameBufferHeader);
    const std::uint64_t sourceDataBytes = Detail::rgbaDataBytes(sourceWidth, sourceHeight);
    if (sourceDataBytes <= maxFrameDataBytes) {
        return { request,
                 static_cast<std::uint32_t>(sourceWidth),
                 static_cast<std::uint32_t>(sourceHeight),
                 static_cast<std::uint32_t>(sourceWidth) * 4U,
                 sourceDataBytes };
    }

    constexpr long double maxPixels = static_cast<long double>(maxFrameDataBytes / 4U);
    const long double sourcePixels = static_cast<long double>(sourceWidth) * static_cast<long double>(sourceHeight);
    const long double scale = std::sqrt(maxPixels / sourcePixels);
    int frameWidth = Detail::scaledDimension(sourceWidth, scale);
    int frameHeight = Detail::scaledDimension(sourceHeight, scale);
    // Avoid floating point edge cases
    while (Detail::rgbaDataBytes(frameWidth, frameHeight) > maxFrameDataBytes) {
        if (frameWidth >= frameHeight && frameWidth > 1)
            --frameWidth;
        else
            --frameHeight;
    }

    Model::RenderRequest fitted = request;
    if (tile) {
        const int tileX = Detail::scaledCoordinate(tile->x, scale);
        const int tileY = Detail::scaledCoordinate(tile->y, scale);
        fitted.width = Detail::scaledDimension(request.width, scale);
        fitted.height = Detail::scaledDimension(request.height, scale);
        fitted.tile = Model::RenderTile { tileX, tileY, frameWidth, frameHeight };
    } else {
        fitted.width = frameWidth;
        fitted.height = frameHeight;
    }

    const std::uint64_t frameDataBytes = Detail::rgbaDataBytes(frameWidth, frameHeight);
    return { std::move(fitted),
             static_cast<std::uint32_t>(frameWidth),
             static_cast<std::uint32_t>(frameHeight),
             static_cast<std::uint32_t>(frameWidth) * 4U,
             frameDataBytes };
}

} // namespace Mu::Worker::Runtime

#endif // MUPDF_WORKER_RUNTIME_RENDER_BUDGET_HPP
