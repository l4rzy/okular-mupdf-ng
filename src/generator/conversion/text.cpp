// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/text.hpp"

#include <QString>
#include <algorithm>

namespace Mu::Generator::Conversion {

Okular::TextPage* textPage(const std::vector<Model::TextBox>& boxes, qreal width, qreal height)
{
    auto* result = new Okular::TextPage();
    if (width <= 0 || height <= 0 || boxes.empty())
        return result;

    const qreal inverseWidth = 1.0 / width;
    const qreal inverseHeight = 1.0 / height;
    for (const auto& box : boxes) {
        if (box.text.empty() || box.right <= 0 || box.left >= width || box.bottom <= 0 || box.top >= height)
            continue;

        qreal left = std::clamp(box.left * inverseWidth, 0.0, 1.0);
        qreal top = std::clamp(box.top * inverseHeight, 0.0, 1.0);
        qreal right = std::clamp(box.right * inverseWidth, 0.0, 1.0);
        qreal bottom = std::clamp(box.bottom * inverseHeight, 0.0, 1.0);

        if (left > right)
            std::swap(left, right);
        if (top > bottom)
            std::swap(top, bottom);

        if (right - left <= 0.0 && bottom - top <= 0.0)
            continue;

        result->append(QString::fromStdString(box.text), Okular::NormalizedRect(left, top, right, bottom));
    }
    return result;
}

Okular::TextPage* ocrTextPage(const QVector<Plugin::Caching::OCR::CacheItem>& boxes)
{
    auto* result = new Okular::TextPage();
    for (const auto& box : boxes) {
        if (box.ch.isEmpty() || box.r <= 0.0 || box.l >= 1.0 || box.b <= 0.0 || box.t >= 1.0)
            continue;

        qreal left = std::clamp(static_cast<qreal>(box.l), 0.0, 1.0);
        qreal top = std::clamp(static_cast<qreal>(box.t), 0.0, 1.0);
        qreal right = std::clamp(static_cast<qreal>(box.r), 0.0, 1.0);
        qreal bottom = std::clamp(static_cast<qreal>(box.b), 0.0, 1.0);

        if (left > right)
            std::swap(left, right);
        if (top > bottom)
            std::swap(top, bottom);

        if (right - left <= 0.0 && bottom - top <= 0.0)
            continue;

        result->append(box.ch, Okular::NormalizedRect(left, top, right, bottom));
    }
    return result;
}

} // namespace Mu::Generator::Conversion
