// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONVERSION_TEXT_HPP
#define MU_GENERATOR_CONVERSION_TEXT_HPP

#include <okular/core/textpage.h>

#include <QString>
#include <QVector>

#include "plugin/ocr/ocr.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

Okular::TextPage* textPage(const std::vector<Model::TextBox>& boxes, qreal width, qreal height);
Okular::TextPage* ocrTextPage(const QVector<Plugin::Caching::OCR::CacheItem>& boxes);
/// Assembles worker text boxes into plain text, preserving source lines
/// (newlines follow endOfLine boxes; empty boxes contribute no text).
QString plainText(const std::vector<Model::TextBox>& boxes);

} // namespace Mu::Generator::Conversion

#endif // MU_GENERATOR_CONVERSION_TEXT_HPP
