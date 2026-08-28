// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_SHARED_TRANSPORT_FRAME_BUFFER_HPP
#define MUPDF_SHARED_TRANSPORT_FRAME_BUFFER_HPP

#include <cstdint>

/// @file frame_buffer.hpp
/// @brief Layout of a per-request shared-memory frame segment.
///
/// Each rendered frame uses one anonymous memfd passed over the native FD
/// channel. The segment contains a FrameBufferHeader followed immediately by
/// the raw pixel bytes.
///
/// Lifecycle:
///  1. Worker creates and maps a memfd, then writes header + pixels.
///  2. Worker passes the FD and transfer ID to the plugin.
///  3. Plugin validates and maps the FD read-only, then closes its received FD.
///     The QImage references the mapping directly; no pixel copy is made.
///  4. Worker closes its local FD and unmaps its local mapping after sending.
///  5. The plugin unmaps the segment when the last QImage copy is destroyed.
///
/// Maximum frame size supported:
///   FRAME_MAX_DATA_BYTES = 128 MiB
/// Frames larger than this are rejected with an error response.

#include "shared/model/types.hpp"
#include "shared/protocol/limits.hpp"

namespace Mu::IPC {

/// Magic number written at byte 0 of every frame SHM segment.
inline constexpr std::uint32_t FRAME_SHM_MAGIC = 0x4D554650u; // 'MUFP'
inline constexpr std::uint32_t FRAME_SHM_VERSION = 1u;

/// Upper bound on the pixel-data portion of a single frame segment.
/// Requests exceeding 128 MiB of RGBA pixel data return an error.
inline constexpr std::uint32_t FRAME_MAX_DATA_BYTES = Limit::FrameMaxDataBytes;

/// @brief Header placed at offset 0 of a per-request frame SHM segment.
///
/// Total size is 64 bytes (one cache line) to keep pixel data cache-aligned.
/// Pixel bytes start at offset sizeof(FrameBufferHeader).
struct alignas(64) FrameBufferHeader {
    uint32_t magic; ///< Must equal FRAME_SHM_MAGIC
    uint32_t version; ///< Must equal FRAME_SHM_VERSION
    uint64_t requestId; ///< Correlation ID matching the control-plane request ID
    uint32_t width; ///< Image width in pixels
    uint32_t height; ///< Image height in pixels
    uint32_t stride; ///< Bytes per row (>= width * 4)
    uint32_t format; ///< Pixel format value (currently RGBA8888 = 1)
    uint8_t _pad[64 - 4 - 4 - 8 - 4 - 4 - 4 - 4]; ///< Reserved, zero-filled
};

static_assert(sizeof(FrameBufferHeader) == 64, "FrameBufferHeader must be exactly 64 bytes (one cache line)");

/// Pointer to the pixel data region within a mapped SHM segment.
inline void* framePixelData(void* base)
{
    return static_cast<char*>(base) + sizeof(FrameBufferHeader);
}

inline const void* framePixelData(const void* base)
{
    return static_cast<const char*>(base) + sizeof(FrameBufferHeader);
}

// ---------------------------------------------------------------------------
// Shared frame validation helpers.
//
// Single source of truth for frame validation so the worker and the plugin can
// never disagree about what makes a frame usable.
// ---------------------------------------------------------------------------

/// @brief Validate a frame SHM header against the expected request geometry.
///
/// Checks magic/version, that the header request ID, dimensions, and pixel
/// format match the render request/response, and that the pixel data fits both
/// FRAME_MAX_DATA_BYTES and the actual mapping size. @p mappedSize is the size
/// of the mapped segment (including the header); when the mapping covers
/// exactly the header the pixel-data region has length zero.
inline bool validateFrameHeader(const FrameBufferHeader* hdr,
                                std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t stride,
                                std::uint32_t format,
                                std::uint64_t requestId,
                                std::uint64_t mappedSize)
{
    if (!hdr)
        return false;
    if (mappedSize < sizeof(*hdr))
        return false;
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t pixelBytes = static_cast<std::uint64_t>(height) * stride;
    const std::uint64_t pixelBudget =
        mappedSize >= sizeof(FrameBufferHeader) ? mappedSize - sizeof(FrameBufferHeader) : 0;
    return width > 0 && height > 0 && stride > 0 && rowBytes <= stride && hdr->magic == FRAME_SHM_MAGIC
        && hdr->version == FRAME_SHM_VERSION && hdr->requestId == requestId && hdr->width == width
        && hdr->height == height && hdr->stride == stride && hdr->format == format && pixelBytes <= FRAME_MAX_DATA_BYTES
        && pixelBytes <= pixelBudget;
}

/// Validates a render-frame descriptor and its mapped shared-memory header.
inline bool validateRenderFrame(const FrameBufferHeader* header,
                                const Model::RenderFrame& frame,
                                std::uint64_t requestId,
                                std::uint64_t mappedSize)
{
    return frame.width > 0 && frame.height > 0 && frame.stride > 0 && frame.format == Model::PixelFormatRgba8888
        && validateFrameHeader(header,
                               static_cast<std::uint32_t>(frame.width),
                               static_cast<std::uint32_t>(frame.height),
                               static_cast<std::uint32_t>(frame.stride),
                               frame.format,
                               requestId,
                               mappedSize);
}

} // namespace Mu::IPC

#endif // MUPDF_SHARED_TRANSPORT_FRAME_BUFFER_HPP
