// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/types.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mupdf/fitz.h>

namespace Mu::Worker::Engine {

inline std::uint32_t readingPaperColor(const Model::DocumentSettings& settings) noexcept
{
    return settings.readingTheme == Model::ReadingTheme::Default ? settings.paperColorRgb : 0xFFFFFF;
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
