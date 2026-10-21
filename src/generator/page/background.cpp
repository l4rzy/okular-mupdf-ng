// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/page/background.hpp"
#include "shared/model/theme_colors.hpp"
#include "shared/protocol/limits.hpp"
#include <QTransform>
#include <okular/core/page.h>

namespace Mu::Generator::Page {

void Background::setTheme(Model::ReadingTheme theme)
{
    const QColor color =
        theme == Model::ReadingTheme::Default ? QColor { } : QColor::fromRgb(Model::ThemeColor::getBackground(theme));
    if (color == m_color)
        return;
    m_color = color;
    // Only the width matters to PagePainter's preview scale limit. A solid
    // strip stretches to any page shape and stays cheap even when Okular
    // rotates cached previews independently for thousands of pages.
    m_preview = m_color.isValid() ? QPixmap(256, 1) : QPixmap { };
    if (!m_preview.isNull()) {
        m_preview.fill(m_color);
        m_rotatedPreview = m_preview.transformed(QTransform().rotate(90));
    } else {
        m_rotatedPreview = { };
    }
}

void Background::seed(Okular::Page& page)
{
    // PagePainter can borrow this pixmap, but hasPixmap(viewObserver) remains
    // false. Copies share one small allocation across every page in the document.
    if (m_preview.isNull())
        page.deletePixmap(this);
    else {
        // setPixmap applies the page rotation asynchronously. Preorient the
        // strip so the resulting cached preview always has its wide edge on X.
        const auto& preview = int(page.rotation()) % 2 ? m_rotatedPreview : m_preview;
        page.setPixmap(this, new QPixmap(preview));
    }
}

void Background::refresh(Okular::Page& page)
{
    // Rotation jobs may finish after deletePixmap(), including after Default
    // was selected. Remove those stale previews without scheduling another job.
    // Checking dimensions also prevents a rotation-notification/reseed loop.
    if (m_preview.isNull() || !page.hasPixmap(this, m_preview.width(), m_preview.height()))
        seed(page);
}

QImage Background::partialImage(QSize size) const
{
    // Tile geometry uses inclusive right/bottom edges, allowing one extra pixel.
    constexpr int maxDimension = Limit::MaxTiledRenderDimension + 1;
    if (!m_color.isValid() || size.width() <= 0 || size.height() <= 0 || size.width() > maxDimension
        || size.height() > maxDimension || qint64(size.width()) * size.height() > Limit::MaxSharedFrameBytes / 4)
        return { };
    QImage image(size, QImage::Format_RGB32);
    if (!image.isNull())
        image.fill(m_color);
    return image;
}

} // namespace Mu::Generator::Page
