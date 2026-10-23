// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_LINK_PREVIEW_POLICY_HPP
#define MU_GENERATOR_LINK_PREVIEW_POLICY_HPP

#include <QSize>
#include <QSizeF>

#include <algorithm>
#include <cmath>

namespace Mu::Generator {

enum class LinkPreviewMode { Off, AutoSize, Small, Large };

struct LinkPreviewPage {
    QSizeF size;
    double fontSize = 0; // Active EPUB/MOBI layout font in points; zero for fixed-layout documents.
};

struct LinkPreviewSize {
    QSize render;
    int visibleHeight = 0;
};

// All UI dimensions are logical pixels. Scale the existing page without reflowing
// it so link destinations and pagination remain valid.
inline LinkPreviewSize sizeLinkPreview(LinkPreviewMode mode, QSize window, QSize screen, const LinkPreviewPage& page)
{
    if (mode == LinkPreviewMode::Off || screen.isEmpty() || !std::isfinite(page.size.width())
        || !std::isfinite(page.size.height()) || page.size.width() <= 0 || page.size.height() <= 0)
        return { };

    int width;
    int height;
    switch (mode) {
    case LinkPreviewMode::Small:
        width = 480;
        height = 300;
        break;
    case LinkPreviewMode::Large:
        width = 640;
        height = 400;
        break;
    case LinkPreviewMode::AutoSize:
        if (window.isEmpty())
            return { };
        width = std::min(800, std::max(1, qRound(window.width() * 0.55)));
        height = std::min(560, std::max(1, qRound(window.height() * 0.55)));
        if (std::isfinite(page.fontSize) && page.fontSize > 0) {
            // Aim for a readable 16px base font. Embedded CSS may override it.
            width = qRound(std::clamp(page.size.width() * (16.0 / page.fontSize), 360.0, 800.0));
            width = std::min(width, std::max(1, qRound(window.width() * 0.70)));
        }
        break;
    default:
        return { };
    }
    width = std::min(width, std::max(1, screen.width() - 48));
    height = std::min(height, std::max(1, screen.height() - 48));
    // Bound allocations even for unusually tall/wide document pages.
    const double aspect = page.size.width() / page.size.height();
    const QSize render = aspect >= static_cast<double>(width) / 4096.0
        ? QSize(width, std::max(1, qRound(width / aspect)))
        : QSize(std::max(1, qRound(4096.0 * aspect)), 4096);
    return { render, std::min(height, render.height()) };
}

} // namespace Mu::Generator

#endif // MU_GENERATOR_LINK_PREVIEW_POLICY_HPP
