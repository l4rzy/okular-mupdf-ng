// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONVERSION_TEXT_HPP
#define MU_GENERATOR_CONVERSION_TEXT_HPP

#include <okular/core/textpage.h>

#include <QString>
#include <QVector>
#include <optional>

#include "plugin/ocr/ocr.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

Okular::TextPage* textPage(const std::vector<Model::TextBox>& boxes, qreal width, qreal height);
/// Keeps native text and adds image OCR; Okular determines the combined reading order.
Okular::TextPage* mergedTextPage(const std::vector<Model::TextBox>& nativeBoxes,
                                 qreal width,
                                 qreal height,
                                 const QVector<Plugin::Caching::OCR::CacheItem>& additions);
/// Assembles worker text boxes into plain text, preserving source lines
/// (newlines follow endOfLine boxes; empty boxes contribute no text).
QString plainText(const std::vector<Model::TextBox>& boxes);
/// Short reading-position context; whitespace and soft hyphens are ignored.
QString captureReflowText(const std::vector<Model::TextBox>& boxes, double top);
/// Returns the first matching box's top only when the snippet is unique.
std::optional<double> findReflowText(const std::vector<Model::TextBox>& boxes, const QString& snippet);

} // namespace Mu::Generator::Conversion

#endif // MU_GENERATOR_CONVERSION_TEXT_HPP
