// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_GENERATOR_CONVERSION_TEXT_HPP
#define MUPDF_GENERATOR_CONVERSION_TEXT_HPP

#include <okular/core/textpage.h>

#include <QVector>

#include "plugin/ocr/ocr.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

Okular::TextPage* textPage(const std::vector<Model::TextBox>& boxes, qreal width, qreal height);
Okular::TextPage* ocrTextPage(const QVector<Plugin::Caching::OCR::CacheItem>& boxes);

} // namespace Mu::Generator::Conversion

#endif
