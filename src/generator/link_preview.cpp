// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/link_preview.hpp"

#include <KLocalizedString>
#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QPointer>
#include <QToolTip>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Mu::Generator {

LinkPreview::LinkPreview(Renderer render, PageSize pageSize)
    : m_render(std::move(render))
    , m_pageSize(std::move(pageSize))
{
    m_pool.setMaxThreadCount(1);
}

LinkPreview::~LinkPreview()
{
    reset();
}

void LinkPreview::reset()
{
    ++m_generation;
    if (m_cancelled)
        m_cancelled->store(true);
    m_pool.waitForDone();
    m_cancelled.reset();
    m_cache.clear();
    if (!m_lastTip.isEmpty() && QToolTip::text() == m_lastTip)
        QToolTip::hideText();
    m_lastTip.clear();
}

QString LinkPreview::tip(const Model::Viewport& destination)
{
    const QString fallback = i18n("Go to page %1", static_cast<qint64>(destination.page) + 1);
    if (destination.page < 0)
        return fallback;
    const QSizeF pageSize = m_pageSize(destination.page);
    if (!std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height()) || pageSize.width() <= 0
        || pageSize.height() <= 0)
        return fallback;

    // Render only a short strip around the destination, at a fixed preview
    // scale. Bound even unusually tall/wide document geometry.
    const double aspect = pageSize.width() / pageSize.height();
    const QSize size = aspect >= 560.0 / 4096.0 ? QSize(560, std::max(1, qRound(560.0 / aspect)))
                                                : QSize(std::max(1, qRound(4096.0 * aspect)), 4096);
    const double y =
        (destination.coordinateMask & Model::Viewport::CoordinateY) != 0 && std::isfinite(destination.normalizedY)
        ? std::clamp(destination.normalizedY, 0.0, 1.0)
        : 0.0;
    const int height = std::min(320, size.height());
    const int top = std::clamp(static_cast<int>(y * size.height()) - 24, 0, size.height() - height);
    const QRect region(0, top, size.width(), height);
    const QString key =
        QStringLiteral("%1:%2:%3:%4").arg(destination.page).arg(size.width()).arg(size.height()).arg(top);
    if (const auto* cached = m_cache.object(key)) {
        m_lastTip = *cached;
        return m_lastTip;
    }
    // Never build a queue of speculative renders while the reader moves over
    // links. A busy or failed preview keeps the normal navigation tooltip.
    if (m_cancelled)
        return fallback;

    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    const auto generation = m_generation;
    const auto render = m_render;
    // Distinguish this pending tooltip from those of other tabs/documents.
    const QString pending = QStringLiteral("<p>%1</p><!--mupdf-preview-%2-%3-%4-->")
                                .arg(fallback.toHtmlEscaped())
                                .arg(reinterpret_cast<quintptr>(this))
                                .arg(generation)
                                .arg(key);
    m_lastTip = pending;
    const QPoint position = QCursor::pos();
    const QPointer<QWidget> widget = QApplication::widgetAt(position);
    m_pool.start([this, render, cancelled, generation, key, pending, position, widget, destination, size, region] {
        const QImage image = render(destination.page, size, region, [cancelled] { return cancelled->load(); });
        QByteArray png;
        QBuffer buffer(&png);
        QString html;
        if (!cancelled->load() && !image.isNull() && image.size() == region.size() && image.save(&buffer, "PNG")) {
            html = pending
                + QStringLiteral("<img src=\"data:image/png;base64,%1\" width=\"%2\" height=\"%3\"/>")
                      .arg(QString::fromLatin1(png.toBase64()))
                      .arg(image.width())
                      .arg(image.height());
        }
        QMetaObject::invokeMethod(this, [this, generation, key, pending, html, position, widget] {
            if (generation != m_generation)
                return;
            m_cancelled.reset();
            if (html.isEmpty())
                return;
            m_cache.insert(key, new QString(html), static_cast<int>(html.size()) * static_cast<int>(sizeof(QChar)));
            // Qt delays hiding after pointer movement. Update only the original
            // stationary hover, while Okular still displays this request's tip.
            if (widget && QCursor::pos() == position && QApplication::widgetAt(position) == widget
                && QToolTip::isVisible() && QToolTip::text() == pending) {
                m_lastTip = html;
                // Okular's original link rectangle is not exposed here. Limit
                // the replacement to the cursor's vicinity so it hides when
                // the reader moves away; cached tips use Okular's own rectangle.
                const QRect hoverArea(widget->mapFromGlobal(position) - QPoint(3, 3), QSize(7, 7));
                QToolTip::showText(position, html, widget, hoverArea);
            }
        });
    });
    return pending;
}

} // namespace Mu::Generator
