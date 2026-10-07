// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/signature_preview.hpp"

#include "generator/config/okular_emblem.hpp"
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLatin1String>
#include <QPainter>

#include <algorithm>

#include "generator/config/signature_emblem.hpp"
#include "plugin/util/signing_timestamp.hpp"
#include "shared/logging.hpp"

namespace Mu::Generator::Config {
namespace {

QString loadSignatureFont()
{
    static const QString family = [] {
        const int fontId =
            QFontDatabase::addApplicationFont(QString::fromUtf8(SIGNATURE_FONT_DIR "/Allura-Regular.ttf"));
        if (fontId < 0) {
            MU_LOG(warning,
                   "Mu::Generator::Config",
                   std::string("could not load signature font from " SIGNATURE_FONT_DIR));
            return QString { };
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (families.isEmpty()) {
            MU_LOG(warning, "Mu::Generator::Config", "signature font exposes no font families");
            return QString { };
        }
        return families.front();
    }();
    return family;
}

} // namespace

SignaturePreview buildSignaturePreview(bool simple, bool useUtc, const QDateTime& now, SignatureEmblem emblem)
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
        return { { }, { name, reason, date }, emblem };
    return { name,
             { QStringLiteral("Digitally signed by %1").arg(name),
               QStringLiteral("DN: %1").arg(distinguishedName),
               QStringLiteral("Reason: %1").arg(reason),
               QStringLiteral("Location: %1").arg(location),
               QStringLiteral("Date: %1").arg(date) },
             emblem };
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
    const QString scriptFamily = loadSignatureFont();
    if (scriptFamily.isEmpty())
        leftFont.setBold(true);
    else {
        leftFont.setFamily(scriptFamily);
        leftFont.setBold(false);
    }
    if (leftFont.pointSizeF() > 0)
        leftFont.setPointSizeF(leftFont.pointSizeF() * 1.5);
    else if (leftFont.pixelSize() > 0)
        leftFont.setPixelSize(static_cast<int>(leftFont.pixelSize() * 1.5));
    const QFontMetrics leftMetrics(leftFont);
    const QRect leftInkBounds = leftMetrics.boundingRect(preview.leftText);
    const int leftMinX = std::min(0, leftInkBounds.left());
    const int leftMaxX = std::max(leftMetrics.horizontalAdvance(preview.leftText), leftInkBounds.right() + 1);
    const int leftWidth = preview.leftText.isEmpty() ? 0 : leftMaxX - leftMinX;
    const int rightHeight = lineHeight * static_cast<int>(lineCount);
    const int leftHeight = preview.leftText.isEmpty() ? 0 : leftInkBounds.height();
    const int contentHeight = std::max(rightHeight, leftHeight);
    const int panesWidth = leftWidth > 0 ? leftWidth + paneGap + rightWidth : rightWidth;

    const QSize logicalSize(panesWidth + 2 * padding + 2, contentHeight + 2 * padding + 2);

    const qreal ratio = devicePixelRatio > 0 ? devicePixelRatio : 1;
    QImage image((logicalSize.toSizeF() * ratio).toSize(), QImage::Format_RGB32);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(QColor(0x9a, 0x9a, 0x9a));
    painter.drawRect(0, 0, logicalSize.width() - 1, logicalSize.height() - 1);
    if (preview.emblem == SignatureEmblem::Okular) {
        const auto emblem = renderOkularEmblem();
        const QSizeF size = emblem.size().scaled(QSize(panesWidth, contentHeight), Qt::KeepAspectRatio);
        const QRectF rect((logicalSize.width() - size.width()) / 2,
                          (logicalSize.height() - size.height()) / 2,
                          size.width(),
                          size.height());
        // Pre-filter the large source at the preview's physical pixel size.
        // QPainter's bilinear transform alone skips fine rims during reduction.
        const auto scaled = emblem.scaled((size * ratio).toSize(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(rect, scaled);
        painter.restore();
    } else if (preview.emblem == SignatureEmblem::MuPDF) {
        const auto path = buildSignatureEmblemPath();
        const auto bounds = path.boundingRect();
        const qreal scale = std::min(panesWidth / bounds.width(), contentHeight / bounds.height());
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(logicalSize.width() / 2.0, logicalSize.height() / 2.0);
        // MuPDF's signature artwork uses an upward Y axis.
        painter.scale(scale, -scale);
        painter.translate(-bounds.center());
        painter.fillPath(path, QColor(0xa4, 0xca, 0xf5));
        painter.restore();
    }
    painter.setPen(Qt::black);
    if (leftWidth > 0) {
        painter.setFont(leftFont);
        const int leftBaseline = padding + (contentHeight - leftHeight) / 2 - leftInkBounds.top();
        painter.drawText(padding - leftMinX, leftBaseline, preview.leftText);
    }
    painter.setFont(font);
    int baseline = padding + (contentHeight - rightHeight) / 2 + metrics.ascent();
    const int rightLeft = padding + (leftWidth > 0 ? leftWidth + paneGap : 0);
    for (const QString& line : preview.rightLines) {
        painter.drawText(rightLeft, baseline, line);
        baseline += metrics.lineSpacing();
    }
    return image;
}

} // namespace Mu::Generator::Config
