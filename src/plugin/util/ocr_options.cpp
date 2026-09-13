// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/ocr_options.hpp"

#include <QLatin1String>

#include "plugin/caching/ocr_constants.hpp"

namespace Mu::Plugin::Util {

namespace OcrConstant = ::Mu::Plugin::OCR::Constant;

QString stripLangSuffix(const QString& lang)
{
    QString clean = lang;
    if (clean.endsWith(QLatin1String(".traineddata")))
        clean.chop(12);
    return clean;
}

float qualityToDpi(int quality)
{
    switch (quality) {
    case 0: // Speed
        return OcrConstant::DPI_FAST;
    case 2: // Accuracy
        return OcrConstant::DPI_ACCURACY;
    default:
        return OcrConstant::DPI_BALANCED;
    }
}

} // namespace Mu::Plugin::Util
