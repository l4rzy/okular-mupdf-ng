// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/placeholder_image.hpp"

#include <KLocalizedString>
#include <QFont>
#include <QPainter>
#include <QtGlobal>

namespace Mu::Generator {

namespace {

struct Palette {
    QColor cardBg;
    QColor cardBorder;
    QColor textColor;
    QString defaultMessage;
};

Palette paletteFor(PlaceholderKind kind)
{
    if (kind == PlaceholderKind::Loading) {
        return {
            QColor(235, 242, 255),
            QColor(60, 110, 190),
            QColor(30, 70, 140),
            i18n("Rendering page..."),
        };
    }
    return {
        QColor(255, 235, 235),
        QColor(180, 45, 45),
        QColor(120, 30, 30),
        i18n("Document renderer unavailable\nRestart Okular to try again."),
    };
}

} // namespace

QImage createPlaceholderImage(int width, int height, PlaceholderKind kind, const QString& message)
{
    if (width <= 0 || height <= 0)
        return { };

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return { };

    image.fill(QColor(245, 245, 245));

    const Palette palette = paletteFor(kind);
    const QString textToDraw = message.isEmpty() ? palette.defaultMessage : message;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    const int minDimension = qMin(width, height);
    const int borderWidth = qMax(1, minDimension / 160);
    const int margin = qMax(2, minDimension / 20);

    painter.setPen(QPen(palette.cardBorder, borderWidth));
    painter.setBrush(palette.cardBg);
    painter.drawRoundedRect(image.rect().adjusted(margin, margin, -margin, -margin), margin, margin);

    // Skip drawing text on tiny thumbnails to avoid illegible artifacts
    if (width < 120 || height < 80)
        return image;

    QFont font = painter.font();
    font.setPixelSize(qBound(10, minDimension / 18, 28));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(palette.textColor);

    const QRect textRect = image.rect().adjusted(margin * 2, margin * 2, -margin * 2, -margin * 2);
    painter.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, textToDraw);
    return image;
}

} // namespace Mu::Generator
