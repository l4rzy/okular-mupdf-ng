// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/ocr_options.hpp"

#include "plugin/caching/ocr_constants.hpp"

namespace Mu::Plugin::Util {

namespace OcrConstant = ::Mu::Plugin::OCR::Constant;

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
