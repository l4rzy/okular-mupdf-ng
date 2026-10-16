// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/types.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mupdf/fitz.h>

namespace Mu::Worker::Engine {

inline std::uint32_t readingPaperColor(const Model::DocumentSettings& settings) noexcept
{
    return settings.readingTheme == Model::ReadingTheme::Default ? settings.paperColorRgb : 0xFFFFFF;
}

inline bool isMostlyDarkPixmap(fz_context* context, fz_pixmap* pixmap)
{
    if (fz_pixmap_components(context, pixmap) != 4)
        fz_throw(context, FZ_ERROR_ARGUMENT, "reading theme requires an RGBA viewing pixmap");
    const int width = fz_pixmap_width(context, pixmap);
    const int height = fz_pixmap_height(context, pixmap);
    const int stride = fz_pixmap_stride(context, pixmap);
    const auto* samples = fz_pixmap_samples(context, pixmap);
    std::size_t darkPixels = 0;
    for (int y = 0; y < height; ++y) {
        const auto* pixel = samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        std::uint32_t darkRow = 0;
        for (int x = 0; x < width; ++x, pixel += 4) {
            // A packed, alignment-safe load lets baseline SSE2 vectorize the reduction.
            std::uint32_t rgba;
            std::memcpy(&rgba, pixel, sizeof(rgba));
            if constexpr (std::endian::native == std::endian::big)
                rgba = std::byteswap(rgba);
            // Approximate perceptual luminance; the maximum sum is 255 * 256.
            const auto luminance =
                static_cast<std::uint16_t>(54 * (rgba & 255) + 183 * ((rgba >> 8) & 255) + 19 * ((rgba >> 16) & 255));
            darkRow += luminance < 128 * 256;
        }
        darkPixels += darkRow;
    }
    return darkPixels > static_cast<std::size_t>(width) * static_cast<std::size_t>(height) / 2;
}

// Always sample the whole page, independently of the requested tile or zoom.
// Keep the thumbnail temporary so edits and layer changes cannot leave stale decisions.
inline bool
isMostlyDarkPage(fz_context* context, fz_page* page, fz_rect bounds, fz_cookie* cookie, fz_display_list* list = nullptr)
{
    fz_pixmap* volatile thumbnail = nullptr;
    fz_device* volatile device = nullptr;
    bool dark = false;
    fz_var(dark);
    fz_try(context)
    {
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        thumbnail = fz_new_pixmap(context, fz_device_rgb(context), 64, 64, nullptr, 1);
        fz_clear_pixmap_with_value(context, thumbnail, 255);
        const fz_matrix transform = fz_concat(fz_translate(-bounds.x0, -bounds.y0),
                                              fz_scale(64 / (bounds.x1 - bounds.x0), 64 / (bounds.y1 - bounds.y0)));
        device = fz_new_draw_device(context, transform, thumbnail);
        if (list)
            fz_run_display_list(context, list, device, fz_identity, fz_infinite_rect, cookie);
        else
            fz_run_page(context, page, device, fz_identity, cookie);
        fz_close_device(context, device);
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        dark = isMostlyDarkPixmap(context, thumbnail);
    }
    fz_always(context)
    {
        fz_drop_device(context, device);
        fz_drop_pixmap(context, thumbnail);
    }
    fz_catch(context) fz_rethrow(context);
    return dark;
}

// Transform the fully composited viewing pixmap, leaving PDF objects and all
// non-viewing paths untouched. Dark mode inverts brightness while retaining
// color distinctions: a 180-degree HSV hue rotation after inversion reduces
// to max+min-rgb before mapping into the theme palette.
inline void
applyReadingTheme(fz_context* context, fz_pixmap* pixmap, Model::ReadingTheme theme, const fz_cookie* cookie)
{
    if (theme == Model::ReadingTheme::Default)
        return;
    if (fz_pixmap_components(context, pixmap) != 4)
        fz_throw(context, FZ_ERROR_ARGUMENT, "reading theme requires an RGBA viewing pixmap");
    const bool dark = theme == Model::ReadingTheme::Dark;
    const std::array<int, 3> background = dark ? std::array { 32, 33, 36 } : std::array { 214, 214, 214 };
    const std::array<int, 3> foreground = dark ? std::array { 218, 220, 224 } : std::array { 36, 36, 36 };
    std::array<std::array<std::uint8_t, 256>, 3> palette { };
    for (std::size_t channel = 0; channel < 3; ++channel)
        for (std::size_t value = 0; value < 256; ++value)
            palette[channel][value] = static_cast<std::uint8_t>(
                foreground[channel] + (background[channel] - foreground[channel]) * static_cast<int>(value) / 255);
    const int width = fz_pixmap_width(context, pixmap);
    const int height = fz_pixmap_height(context, pixmap);
    const int stride = fz_pixmap_stride(context, pixmap);
    auto* samples = fz_pixmap_samples(context, pixmap);
    for (int y = 0; y < height; ++y) {
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        auto* pixel = samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < width; ++x, pixel += 4) {
            // View renderers composite onto an opaque background before this pass.
            const int sum = dark
                ? int(std::min({ pixel[0], pixel[1], pixel[2] })) + int(std::max({ pixel[0], pixel[1], pixel[2] }))
                : 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
                pixel[channel] =
                    palette[channel][static_cast<std::size_t>(dark ? sum - pixel[channel] : pixel[channel])];
        }
    }
}

} // namespace Mu::Worker::Engine
