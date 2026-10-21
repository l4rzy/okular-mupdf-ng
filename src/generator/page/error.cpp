// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/page/error.hpp"

#include <KLocalizedString>
#include <QFont>
#include <QFontMetrics>
#include <QImageReader>
#include <QPainter>
#include <QtGlobal>

namespace Mu::Generator::Page {

namespace {

QImage drawErrorCard(int width, int height, const QString& message)
{
    if (width <= 0 || height <= 0)
        return { };

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return { };

    image.fill(QColor(243, 244, 246));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const int minDimension = qMin(width, height);
    // Keep thumbnails simple instead of squeezing guidance into illegible text.
    if (width < 120 || height < 80)
        return image;

    const int margin = qBound(8, minDimension / 24, 32);
    const int padding = qBound(12, minDimension / 24, 28);
    const int cardWidth = qMin(640, width - margin * 2);
    const int textWidth = cardWidth - padding * 2;
    const bool logoOnly = width < 240 || height < 180;
    const int logoSize = logoOnly ? qMin(64, minDimension - 16) : qBound(32, cardWidth / 9, 64);
    // Rasterize the SVG at 4x size, then use smooth image downsampling rather
    // than the painter's bilinear sampling, which aliases fine logo details.
    QImageReader logoReader(QStringLiteral(MUPDFNG_LOGO_PATH));
    logoReader.setScaledSize(QSize(logoSize * 4, logoSize * 4));
    QImage logo = logoReader.read();
    if (!logo.isNull())
        logo = logo.scaled(logoSize, logoSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (logoOnly) {
        painter.drawImage(QRect((width - logoSize) / 2, (height - logoSize) / 2, logoSize, logoSize), logo);
        return image;
    }
    const int gap = qBound(8, minDimension / 50, 16);
    QFont font = painter.font();
    font.setPixelSize(qBound(12, cardWidth / 32, 17));
    const int headingSize = qBound(16, cardWidth / 24, 23);
    QFont headingFont = font;
    headingFont.setPixelSize(headingSize);
    headingFont.setBold(true);
    const QString heading = i18n("Document unavailable");
    const int textFlags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap;
    const int headingHeight =
        QFontMetrics(headingFont).boundingRect(QRect(0, 0, textWidth, 0), textFlags, heading).height();
    const int headerHeight = logoSize + gap * 2 + headingHeight;
    const int availableHeight = qMax(0, height - margin * 2 - padding * 2 - headerHeight);
    int textHeight = 0;
    do {
        textHeight = QFontMetrics(font).boundingRect(QRect(0, 0, textWidth, 0), textFlags, message).height();
        if (textHeight <= availableHeight || font.pixelSize() <= 8)
            break;
        font.setPixelSize(font.pixelSize() - 1);
    } while (true);
    const int cardHeight = qMin(height - margin * 2, padding * 2 + headerHeight + textHeight);
    const QRect card((width - cardWidth) / 2, (height - cardHeight) / 2, cardWidth, cardHeight);
    painter.setPen(QPen(QColor(180, 45, 45), 2));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(card, 12, 12);

    const int left = card.left() + padding;
    int top = card.top() + padding;
    painter.drawImage(QRect(left, top, logoSize, logoSize), logo);
    painter.setFont(headingFont);
    painter.setPen(QColor(39, 46, 52));
    const int logoSpace = logo.isNull() ? 0 : logoSize + gap;
    painter.drawText(QRect(left + logoSpace, top, textWidth - logoSpace, logoSize),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("MuPDF-NG"));
    top += logoSize + gap;
    painter.setPen(QColor(139, 86, 17));
    painter.drawText(QRect(left, top, textWidth, headingHeight), textFlags, heading);
    top += headingHeight + gap;
    painter.setFont(font);
    painter.setPen(QColor(61, 68, 75));
    painter.drawText(QRect(left, top, textWidth, qMax(0, card.bottom() - padding - top + 1)), textFlags, message);
    return image;
}

} // namespace

void Error::activate(QString message, Reason reason)
{
    // Terminal state is sticky: never re-notify or downgrade WorkerUnavailable.
    if (isActive() && m_reason == Reason::WorkerUnavailable)
        return;

    m_reason = reason;
    // Atomic shared_ptr publication: readers either see nothing (inactive) or
    // a fully constructed string.
    m_message.store(std::make_shared<const QString>(std::move(message)));
}

bool Error::deactivate()
{
    // The published message stays until the queued reopen completes and
    // reset() runs, keeping renders blocked in the meantime. Repeated
    // deactivations must not request a duplicate reopen.
    if (!isActive() || m_reason != Reason::SandboxGate || m_reloadQueued)
        return false;
    m_reloadQueued = true;
    return true;
}

void Error::reset()
{
    // Terminal failure persists for the owning-thread lifetime.
    if (m_reason == Reason::WorkerUnavailable)
        return;
    m_message.store(nullptr);
    m_reloadQueued = false;
}

QString Error::message() const
{
    const auto snapshot = m_message.load();
    return snapshot ? *snapshot : QString();
}

QImage Error::image(int width, int height) const
{
    return drawErrorCard(width, height, message());
}

} // namespace Mu::Generator::Page
