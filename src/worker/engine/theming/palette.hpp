// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
inline constexpr unsigned PageDarknessCutoff = 128;
inline constexpr unsigned ImageDarknessCutoff = 160;
inline constexpr unsigned DarkCoverageDivisor = 2; // Require more than half of the pixels or alpha weight.

// Retain 80% image brightness, rounding to the nearest integer.
inline constexpr int ImageBrightnessNumerator = 4;
inline constexpr int ImageBrightnessDenominator = 5;

// Share the same 40% saturation blend between vector and bitmap paths.
inline constexpr int SaturationNumerator = 2;
inline constexpr int SaturationDenominator = 5;
inline constexpr float SaturationBlend = static_cast<float>(SaturationNumerator) / SaturationDenominator;

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

/// Dusk and Dark inspect page content before choosing how to render the theme.
constexpr bool shouldInspectThemePage(Model::DocumentTheme theme) noexcept
{
    return theme == Model::DocumentTheme::Dusk || theme == Model::DocumentTheme::Dark;
}

/// Selects the drawing device, vector recoloring, and initial background.
/// When useDevice is false, apply the theme to the completed bitmap instead.
ThemeRenderPlan planThemeRendering(const Model::DocumentSettings& settings, ThemePageInfo page) noexcept;

std::uint32_t readingPaperColor(const Model::DocumentSettings& settings) noexcept;

bool isMostlyDarkPixmap(fz_context* context, fz_pixmap* pixmap);
ThemePageInfo inspectThemePage(fz_context* context,
                               fz_page* page,
                               fz_rect bounds,
                               Model::DocumentTheme theme,
                               fz_cookie* cookie,
                               fz_display_list* list = nullptr);

// Dim premultiplied RGBA to 80%, retaining alpha.
void dimImageRow(unsigned char* pixel, int width) noexcept;

// Transform an RGBA viewing pixmap, preserving premultiplied alpha.
void applyDocumentTheme(fz_context* context, fz_pixmap* pixmap, Model::DocumentTheme theme, const fz_cookie* cookie);

} // namespace Mu::Worker::Engine
