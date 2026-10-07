// SPDX-FileCopyrightText: 2004-2026 Artifex Software, Inc.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// MuPDF signature emblem geometry from source/pdf/pdf-appearance.c.
// See thirdparty/mupdf-COPYING for the license.
#ifndef MU_GENERATOR_CONFIG_SIGNATURE_EMBLEM_HPP
#define MU_GENERATOR_CONFIG_SIGNATURE_EMBLEM_HPP

#include <QPainterPath>

namespace Mu::Generator::Config {

inline QPainterPath buildSignatureEmblemPath()
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);

    path.moveTo(122.25, 0.0);
    path.lineTo(122.25, 14.249);
    path.cubicTo(125.98, 13.842, 129.73, 13.518, 133.5, 13.277);
    path.lineTo(133.5, 0.0);
    path.lineTo(122.25, 0.0);
    path.closeSubpath();
    path.moveTo(140.251, 0.0);
    path.lineTo(140.251, 12.935);
    path.cubicTo(152.534, 12.477, 165.03, 12.899, 177.75, 14.249);
    path.lineTo(177.75, 21.749);
    path.cubicTo(165.304, 20.413, 152.809, 19.871, 140.251, 20.348);
    path.lineTo(140.251, 39.0);
    path.lineTo(133.5, 39.0);
    path.lineTo(133.5, 20.704);
    path.cubicTo(129.756, 20.956, 126.006, 21.302, 122.25, 21.749);
    path.lineTo(122.25, 50.999);
    path.lineTo(177.751, 50.999);
    path.lineTo(177.751, 0.0);
    path.lineTo(140.251, 0.0);
    path.closeSubpath();
    path.moveTo(23.482, 129.419);
    path.cubicTo(-20.999, 199.258, -0.418, 292.039, 69.42, 336.519);
    path.cubicTo(139.259, 381.0, 232.04, 360.419, 276.52, 290.581);
    path.cubicTo(321.001, 220.742, 300.42, 127.961, 230.582, 83.481);
    path.cubicTo(160.743, 39.0, 67.962, 59.581, 23.482, 129.419);
    path.closeSubpath();
    path.moveTo(254.751, 128.492);
    path.cubicTo(303.074, 182.82, 295.364, 263.762, 237.541, 309.165);
    path.cubicTo(179.718, 354.568, 93.57, 347.324, 45.247, 292.996);
    path.cubicTo(-3.076, 238.668, 4.634, 157.726, 62.457, 112.323);
    path.cubicTo(120.28, 66.92, 206.428, 74.164, 254.751, 128.492);
    path.closeSubpath();
    path.moveTo(111.0, 98.999);
    path.cubicTo(87.424, 106.253, 68.25, 122.249, 51.75, 144.749);
    path.lineTo(103.5, 297.749);
    path.lineTo(213.75, 298.499);
    path.cubicTo(206.25, 306.749, 195.744, 311.478, 185.25, 314.249);
    path.cubicTo(164.22, 319.802, 141.22, 319.775, 120.0, 314.999);
    path.cubicTo(96.658, 309.745, 77.25, 298.499, 55.5, 283.499);
    path.cubicTo(69.75, 299.249, 84.617, 311.546, 102.75, 319.499);
    path.cubicTo(117.166, 325.822, 133.509, 327.689, 149.25, 327.749);
    path.cubicTo(164.21, 327.806, 179.924, 326.532, 193.5, 320.249);
    path.cubicTo(213.95, 310.785, 232.5, 294.749, 245.25, 276.749);
    path.lineTo(227.25, 276.749);
    path.cubicTo(213.963, 276.749, 197.25, 263.786, 197.25, 250.499);
    path.lineTo(197.25, 112.499);
    path.cubicTo(213.75, 114.749, 228.0, 127.499, 241.5, 140.999);
    path.cubicTo(231.75, 121.499, 215.175, 109.723, 197.25, 101.249);
    path.cubicTo(181.5, 95.249, 168.412, 94.775, 153.0, 94.499);
    path.cubicTo(139.42, 94.256, 120.75, 95.999, 111.0, 98.999);
    path.closeSubpath();
    path.moveTo(125.25, 105.749);
    path.lineTo(125.25, 202.499);
    path.lineTo(95.25, 117.749);
    path.cubicTo(105.75, 108.749, 114.0, 105.749, 125.25, 105.749);
    path.closeSubpath();
    return path;
}

} // namespace Mu::Generator::Config

#endif
