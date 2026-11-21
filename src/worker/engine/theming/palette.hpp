// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/theme_colors.hpp"
#include "shared/model/types.hpp"
#include <cstdint>
#include <mupdf/fitz.h>

namespace Mu::Worker::Engine {

namespace Theme {

inline constexpr int ThumbnailSize = 64;

// Integer perceptual luminance, scaled by the sum of the channel weights.
inline constexpr unsigned RedWeight = 54;
inline constexpr unsigned GreenWeight = 183;
inline constexpr unsigned BlueWeight = 19;
inline constexpr unsigned LuminanceScale = RedWeight + GreenWeight + BlueWeight;
inline constexpr unsigned ImageDimmingPercent = 80;
inline constexpr unsigned VectorBlendPercent = 40;

} // namespace Theme

struct ThemePageInfo {
    bool mostlyDark = false;
    bool needsCompositing = false;
};

struct ThemeRenderPlan {
    bool useDevice = false;
    bool recolorVectors = false;
    std::uint32_t paperColor = 0;
};

/// Dusk, Dark, and custom profiles inspect page content before choosing how to render the theme.
constexpr bool shouldInspectThemePage(Model::DocumentTheme theme) noexcept
{
    return theme == Model::DocumentTheme::Dusk || theme == Model::DocumentTheme::Dark
        || Model::ThemeColor::isCustom(theme);
}

/// Selects the drawing device, vector recoloring, and initial background.
/// When useDevice is false, apply the theme to the completed bitmap instead.
ThemeRenderPlan planThemeRendering(const Model::DocumentSettings& settings, ThemePageInfo page) noexcept;

std::uint32_t readingPaperColor(const Model::DocumentSettings& settings) noexcept;

bool isMostlyDarkPixmap(fz_context* context, fz_pixmap* pixmap, const Model::ThemeCustomization& custom = { });
ThemePageInfo inspectThemePage(fz_context* context,
                               fz_page* page,
                               fz_rect bounds,
                               Model::DocumentTheme theme,
                               fz_cookie* cookie,
                               fz_display_list* list = nullptr,
                               const Model::ThemeCustomization& custom = { });

// Dim premultiplied RGBA by the requested percentage, retaining alpha.
void dimImageRow(unsigned char* pixel, int width, unsigned dimmingPercent = Theme::ImageDimmingPercent) noexcept;

// Transform an RGBA viewing pixmap, preserving premultiplied alpha.
void applyDocumentTheme(fz_context* context,
                        fz_pixmap* pixmap,
                        Model::DocumentTheme theme,
                        const fz_cookie* cookie,
                        const Model::ThemeCustomization& custom = { });

} // namespace Mu::Worker::Engine
