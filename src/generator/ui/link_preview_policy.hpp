// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_LINK_PREVIEW_POLICY_HPP
#define MU_GENERATOR_LINK_PREVIEW_POLICY_HPP

#include <QSize>
#include <QSizeF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Mu::Generator {

enum class LinkPreviewMode { Off, AdaptiveSize, Small, Large };

struct LinkPreviewPage {
    QSizeF size;
};

struct LinkPreviewSize {
    QSize render;
    int visibleHeight = 0;
    double scale = 0;
};

// All UI dimensions are logical pixels. Scale the existing page without reflowing
// it so link destinations and pagination remain valid.
inline LinkPreviewSize sizeLinkPreview(LinkPreviewMode mode,
                                       QSize window,
                                       QSize screen,
                                       const LinkPreviewPage& page,
                                       double previewScale = 0,
                                       QSize sessionScreen = { })
{
    if (mode == LinkPreviewMode::Off || screen.isEmpty() || !std::isfinite(page.size.width())
        || !std::isfinite(page.size.height()) || page.size.width() <= 0 || page.size.height() <= 0)
        return { };

    int width;
    int height;
    const QSize presetScreen = sessionScreen.isEmpty() ? screen : sessionScreen;
    switch (mode) {
    case LinkPreviewMode::Small:
    case LinkPreviewMode::Large: {
        // Keep the whole popup, including its heading and margins, in the fraction.
        const int divisor = mode == LinkPreviewMode::Small ? 4 : 3;
        width = std::max(1, presetScreen.width() / divisor - 12);
        height = std::max(1, presetScreen.height() / divisor - 48);
        break;
    }
    case LinkPreviewMode::AdaptiveSize:
        if (window.isEmpty())
            return { };
        width = std::max(1, qRound(window.width() * 0.70));
        height = std::max(1, qRound(window.height() * 0.55));
        // Reserve the popup's border, margins and heading inside the screen cap.
        width = std::min(width, std::max(1, screen.width() / 2 - 12));
        height = std::min(height, std::max(1, screen.height() / 2 - 48));
        if (!std::isfinite(previewScale) || previewScale <= 0)
            previewScale = window.width() * 0.55 / page.size.width();
        break;
    default:
        return { };
    }
    width = std::min({ width, 4096, std::max(1, screen.width() - 48) });
    height = std::min(height, std::max(1, screen.height() - 48));
    // Bound allocations even for unusually tall/wide document pages.
    double scale =
        std::min({ width / page.size.width(), 4096.0 / page.size.height(), std::numeric_limits<double>::max() });
    if (mode == LinkPreviewMode::AdaptiveSize)
        scale = std::min(scale, previewScale);
    const QSize render(std::clamp(qRound(page.size.width() * scale), 1, width),
                       std::clamp(qRound(page.size.height() * scale), 1, 4096));
    return { render, std::min(height, render.height()), scale };
}

} // namespace Mu::Generator

#endif // MU_GENERATOR_LINK_PREVIEW_POLICY_HPP
