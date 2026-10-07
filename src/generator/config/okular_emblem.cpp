// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "generator/config/okular_emblem.hpp"
#include "shared/protocol/limits.hpp"

#include <QBuffer>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace Mu::Generator::Config {
namespace {

constexpr int EmblemCanvasSize = 100;
constexpr int EmblemPixelSize = 1024;
constexpr int EmblemOpacity = 56;
static_assert(EmblemPixelSize <= Limit::MaxSignatureEmblemDimension);

// The tilted page, blue crescent, and reading glasses follow Okular's icon.
// Coordinates use a 100x100 canvas; gradients and layered strokes give the
// small signature watermark the same paper and metallic-glasses cues.
void paintOkularEmblem(QPainter& painter)
{
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath page;
    page.moveTo(10, 14);
    page.lineTo(75, 1);
    page.quadTo(78, 0, 79, 3);
    page.lineTo(95, 84);
    page.quadTo(96, 87, 93, 88);
    page.lineTo(28, 100);
    page.quadTo(25, 100, 25, 97);
    page.lineTo(8, 18);
    page.quadTo(7, 15, 10, 14);
    painter.save();
    painter.translate(1, 1);
    painter.fillPath(page, QColor(0, 0, 0, 45));
    painter.restore();
    painter.fillPath(page, QColor(25, 25, 25));

    QPainterPath paper;
    paper.moveTo(11, 16);
    paper.lineTo(76, 3);
    paper.lineTo(92, 84);
    paper.lineTo(28, 97);
    paper.closeSubpath();
    QLinearGradient paperShade(14, 20, 87, 80);
    paperShade.setColorAt(0, QColor(218, 218, 218));
    paperShade.setColorAt(0.52, Qt::white);
    paperShade.setColorAt(1, QColor(237, 237, 237));
    painter.fillPath(paper, paperShade);
    painter.strokePath(paper, QPen(Qt::white, 0.7));

    QPainterPath crescent;
    crescent.moveTo(42, 10);
    crescent.lineTo(54, 7.6);
    crescent.cubicTo(40, 26, 36, 48, 45, 57);
    crescent.cubicTo(57, 70, 75, 50, 83, 40);
    crescent.lineTo(85, 51);
    crescent.cubicTo(68, 70, 46, 67, 37, 58);
    crescent.cubicTo(24, 46, 35, 20, 42, 10);
    crescent.closeSubpath();
    QLinearGradient blue(37, 14, 79, 65);
    blue.setColorAt(0, QColor(54, 157, 215));
    blue.setColorAt(1, QColor(32, 116, 166));
    painter.fillPath(crescent, blue);

    // Temples sit behind the transparent lenses. Their rounded silhouettes
    // and dark gradient distinguish them from the narrow silver front bar.
    QPainterPath leftTemple;
    leftTemple.moveTo(7, 95);
    leftTemple.cubicTo(22, 79, 41, 70, 75, 61);
    leftTemple.lineTo(77, 65);
    leftTemple.cubicTo(45, 74, 25, 85, 10, 98);
    leftTemple.quadTo(6, 99, 7, 95);
    QPainterPath rightTemple;
    rightTemple.moveTo(23, 75);
    rightTemple.cubicTo(49, 65, 73, 63, 99, 73);
    rightTemple.lineTo(98, 77);
    rightTemple.cubicTo(72, 69, 51, 70, 24, 79);
    rightTemple.closeSubpath();
    QLinearGradient charcoal(0, 65, 0, 96);
    charcoal.setColorAt(0, QColor(100, 100, 100));
    charcoal.setColorAt(0.4, QColor(43, 43, 43));
    charcoal.setColorAt(1, QColor(21, 21, 21));
    painter.fillPath(leftTemple, charcoal);
    painter.fillPath(rightTemple, charcoal);

    QPainterPath bar;
    bar.moveTo(3, 81);
    bar.lineTo(94, 57);
    painter.strokePath(bar, QPen(QColor(115, 115, 115), 2.2, Qt::SolidLine, Qt::RoundCap));
    painter.strokePath(bar, QPen(QColor(220, 220, 220), 1.3, Qt::SolidLine, Qt::RoundCap));
    painter.strokePath(bar, QPen(QColor(250, 250, 250), 0.35, Qt::SolidLine, Qt::RoundCap));

    QPainterPath leftLens;
    leftLens.moveTo(7, 76);
    leftLens.cubicTo(4, 65, 37, 60, 41, 68);
    leftLens.cubicTo(52, 86, 13, 97, 7, 76);
    leftLens.closeSubpath();
    QPainterPath rightLens;
    rightLens.moveTo(55, 65);
    rightLens.cubicTo(52, 54, 86, 47, 89, 57);
    rightLens.cubicTo(99, 78, 60, 87, 55, 65);
    rightLens.closeSubpath();

    QLinearGradient glass(0, 54, 0, 90);
    glass.setColorAt(0, QColor(255, 255, 255, 12));
    glass.setColorAt(0.65, QColor(233, 239, 241, 65));
    glass.setColorAt(1, QColor(160, 167, 169, 85));
    QLinearGradient metal(0, 53, 0, 93);
    metal.setColorAt(0, QColor(75, 75, 75));
    metal.setColorAt(0.28, QColor(232, 232, 232));
    metal.setColorAt(0.55, QColor(118, 118, 118));
    metal.setColorAt(0.8, QColor(247, 247, 247));
    metal.setColorAt(1, QColor(95, 95, 95));
    for (const auto& lens : { leftLens, rightLens }) {
        painter.save();
        painter.translate(0.6, 0.9);
        painter.strokePath(lens, QPen(QColor(0, 0, 0, 35), 1.8));
        painter.restore();
        painter.fillPath(lens, glass);
        painter.strokePath(lens, QPen(QColor(73, 73, 73), 1.2));
        painter.strokePath(lens, QPen(QBrush(metal), 0.8));
        painter.strokePath(lens, QPen(QColor(255, 255, 255, 180), 0.2));
    }

    QPainterPath bridge;
    bridge.moveTo(42, 70);
    bridge.cubicTo(45, 67, 50, 65, 55, 66);
    painter.strokePath(bridge, QPen(QColor(100, 100, 100), 1.0));
    painter.strokePath(bridge, QPen(QColor(230, 230, 230), 0.45));

    QPainterPath glints;
    glints.moveTo(9, 72);
    glints.cubicTo(14, 67, 30, 63, 36, 65);
    glints.moveTo(59, 59);
    glints.cubicTo(65, 55, 79, 52, 85, 54);
    painter.strokePath(glints, QPen(QColor(255, 255, 255, 210), 0.45, Qt::SolidLine, Qt::RoundCap));
}

} // namespace

QImage renderOkularEmblem()
{
    static const QImage image = [] {
        QImage watermark(EmblemPixelSize, EmblemPixelSize, QImage::Format_ARGB32_Premultiplied);
        watermark.fill(Qt::transparent);
        QPainter painter(&watermark);
        painter.scale(qreal(watermark.width()) / EmblemCanvasSize, qreal(watermark.height()) / EmblemCanvasSize);
        paintOkularEmblem(painter);
        // Apply opacity to the finished image so overlapping layers stay uniform.
        painter.resetTransform();
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.fillRect(watermark.rect(), QColor(0, 0, 0, EmblemOpacity));
        painter.end();
        return watermark;
    }();
    return image;
}

std::vector<std::uint8_t> encodeOkularEmblem()
{
    static const std::vector<std::uint8_t> png = [] {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly) || !renderOkularEmblem().save(&buffer, "PNG"))
            return std::vector<std::uint8_t> { };
        const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        return std::vector<std::uint8_t>(begin, begin + bytes.size());
    }();
    return png;
}

} // namespace Mu::Generator::Config
