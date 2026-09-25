// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/signature_preview.hpp"

#include <QFontMetrics>
#include <QLatin1String>
#include <QPainter>

#include <algorithm>

#include "plugin/util/signing_timestamp.hpp"

namespace Mu::Generator::Config {

SignaturePreview buildSignaturePreview(bool simple, bool useUtc, const QDateTime& now)
{
    // Sample identity: no certificate exists at settings time. Prefixes stay
    // English to match MuPDF's real appearance output.
    constexpr QLatin1String name("Jane Doe");
    constexpr QLatin1String distinguishedName("CN=Jane Doe, O=Example");
    constexpr QLatin1String reason("Approved");
    constexpr QLatin1String location("Berlin");
    const QDateTime stamped = useUtc ? now.toUTC() : now.toLocalTime();
    const QString date = Plugin::Util::SigningTimestamp::displayDate(stamped);
    if (simple)
        return { { }, { name, reason, date } };
    return { name,
             { QStringLiteral("Digitally signed by %1").arg(name),
               QStringLiteral("DN: %1").arg(distinguishedName),
               QStringLiteral("Reason: %1").arg(reason),
               QStringLiteral("Location: %1").arg(location),
               QStringLiteral("Date: %1").arg(date) } };
}

QImage renderSignaturePreview(const SignaturePreview& preview, const QFont& font, qreal devicePixelRatio)
{
    constexpr int padding = 10;
    constexpr int paneGap = 16;
    const QFontMetrics metrics(font);
    int rightWidth = 0;
    for (const QString& line : preview.rightLines)
        rightWidth = std::max(rightWidth, metrics.horizontalAdvance(line));
    const int lineHeight = metrics.lineSpacing();
    const qsizetype lineCount = std::max<qsizetype>(preview.rightLines.size(), 1);

    QFont leftFont(font);
    leftFont.setBold(true);
    if (leftFont.pointSizeF() > 0)
        leftFont.setPointSizeF(leftFont.pointSizeF() * 1.3);
    else if (leftFont.pixelSize() > 0)
        leftFont.setPixelSize(static_cast<int>(leftFont.pixelSize() * 1.3));
    const QFontMetrics leftMetrics(leftFont);
    const int leftWidth = preview.leftText.isEmpty() ? 0 : leftMetrics.horizontalAdvance(preview.leftText);
    const int panesWidth = leftWidth > 0 ? leftWidth + paneGap + rightWidth : rightWidth;

    const QSize logicalSize(panesWidth + 2 * padding + 2, static_cast<int>(lineHeight * lineCount + 2 * padding + 2));

    const qreal ratio = devicePixelRatio > 0 ? devicePixelRatio : 1;
    QImage image((logicalSize.toSizeF() * ratio).toSize(), QImage::Format_RGB32);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(QColor(0x9a, 0x9a, 0x9a));
    painter.drawRect(0, 0, logicalSize.width() - 1, logicalSize.height() - 1);
    painter.setPen(Qt::black);
    if (leftWidth > 0) {
        painter.setFont(leftFont);
        const int textTop = padding + (logicalSize.height() - 2 * padding - leftMetrics.height()) / 2;
        painter.drawText(padding, textTop + leftMetrics.ascent(), preview.leftText);
    }
    painter.setFont(font);
    int baseline = padding + metrics.ascent();
    const int rightLeft = padding + (leftWidth > 0 ? leftWidth + paneGap : 0);
    for (const QString& line : preview.rightLines) {
        painter.drawText(rightLeft, baseline, line);
        baseline += metrics.lineSpacing();
    }
    return image;
}

} // namespace Mu::Generator::Config
