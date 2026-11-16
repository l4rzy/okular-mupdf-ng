// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/text.hpp"

#include <QString>
#include <algorithm>
#include <cmath>
#include <memory>

namespace Mu::Generator::Conversion {
namespace {

struct TextGlyph {
    QString text;
    Okular::NormalizedRect area;
    bool endOfLine;
};

struct TextLine {
    std::size_t begin;
    std::size_t end;
    Okular::NormalizedRect area;
    std::size_t order;
};

std::optional<Okular::NormalizedRect>
normalizeTextArea(double left, double top, double right, double bottom, qreal width, qreal height)
{
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom))
        return std::nullopt;
    if (left > right)
        std::swap(left, right);
    if (top > bottom)
        std::swap(top, bottom);
    if (right <= 0.0 || left >= width || bottom <= 0.0 || top >= height)
        return std::nullopt;
    // Divide directly: the reciprocal of a finite, tiny dimension can overflow.
    Okular::NormalizedRect area(std::clamp(left / width, 0.0, 1.0),
                                std::clamp(top / height, 0.0, 1.0),
                                std::clamp(right / width, 0.0, 1.0),
                                std::clamp(bottom / height, 0.0, 1.0));
    if (area.left == area.right && area.top == area.bottom)
        return std::nullopt;
    return area;
}

bool sharesTextRow(const Okular::NormalizedRect& area, const Okular::NormalizedRect& next)
{
    const qreal overlap = std::min(area.bottom, next.bottom) - std::max(area.top, next.top);
    const qreal height = std::min(area.bottom - area.top, next.bottom - next.top);
    return overlap > 0.0 && overlap >= 0.6 * height;
}

bool followsOnRow(const Okular::NormalizedRect& area, const Okular::NormalizedRect& next)
{
    return next.left > area.left && sharesTextRow(area, next);
}

// An empty permutation means the glyphs already have their source order.
std::vector<std::size_t> orderWordRows(const std::vector<TextGlyph>& glyphs)
{
    std::vector<TextLine> lines;
    std::vector<std::size_t> words;
    for (std::size_t begin = 0; begin < glyphs.size();) {
        std::size_t end = begin;
        auto area = glyphs[begin].area;
        bool singleRow = true;
        bool singleWord = true;
        bool hasText = false;
        bool trailingSpace = false;
        do {
            if (singleWord) {
                for (const auto ch : glyphs[end].text) {
                    if (ch.isSpace()) {
                        trailingSpace = hasText;
                    } else {
                        singleWord = singleWord && !trailingSpace;
                        hasText = true;
                    }
                }
            }
            singleRow = singleRow && sharesTextRow(glyphs[begin].area, glyphs[end].area);
            area |= glyphs[end].area;
        } while (!glyphs[end++].endOfLine && end < glyphs.size());
        if (singleRow && singleWord && hasText)
            words.push_back(lines.size());
        lines.push_back({ begin, end, area, lines.size() });
        begin = end;
    }
    if (words.size() < 2)
        return { };

    // Some documents paint single-word table columns separately. Put aligned
    // cells next to one another before joining their layout regions. Paragraph
    // lines retain their source order for Okular's column analysis.
    std::sort(words.begin(), words.end(), [&lines](auto first, auto second) {
        return lines[first].area.top < lines[second].area.top;
    });
    bool hasWordRows = false;
    for (std::size_t begin = 0; begin < words.size();) {
        std::size_t end = begin + 1;
        while (end < words.size() && sharesTextRow(lines[words[begin]].area, lines[words[end]].area))
            ++end;
        std::sort(words.data() + begin, words.data() + end, [&lines](auto first, auto second) {
            return lines[first].area.left < lines[second].area.left;
        });
        for (std::size_t first = begin; first < end;) {
            std::size_t last = first + 1;
            auto order = words[first];
            while (last < end && lines[words[last - 1]].area.right < lines[words[last]].area.left) {
                order = std::min(order, words[last]);
                ++last;
            }
            for (auto index = first; index < last; ++index)
                lines[words[index]].order = order;
            hasWordRows = hasWordRows || last - first > 1;
            first = last;
        }
        begin = end;
    }
    if (!hasWordRows)
        return { };

    // Anchor each row at its earliest source cell, then read its cells left to
    // right. Other lines keep their source positions.
    std::sort(lines.begin(), lines.end(), [](const TextLine& first, const TextLine& second) {
        return first.order != second.order ? first.order < second.order : first.area.left < second.area.left;
    });
    std::vector<std::size_t> order;
    order.reserve(glyphs.size());
    for (const auto& line : lines) {
        for (auto index = line.begin; index < line.end; ++index)
            order.push_back(index);
    }
    return order;
}

QString normalizeReflowText(QString text)
{
    text.removeIf([](QChar ch) { return ch.isSpace() || ch == QChar(0x00ad); });
    return text;
}

} // namespace

Okular::TextPage* textPage(const std::vector<Model::TextBox>& boxes, qreal width, qreal height)
{
    auto result = std::make_unique<Okular::TextPage>();
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 || boxes.empty())
        return result.release();

    std::vector<TextGlyph> glyphs;
    glyphs.reserve(boxes.size());
    for (const auto& box : boxes) {
        if (box.text.empty())
            continue;
        const auto area = normalizeTextArea(box.left, box.top, box.right, box.bottom, width, height);
        if (!area)
            continue;

        QString text = QString::fromStdString(box.text);
        // Okular removes standalone ASCII spaces before reconstructing words.
        // Keep source spaces with the preceding glyph so they survive that pass.
        if (text == QLatin1String(" ") && !glyphs.empty() && !glyphs.back().endOfLine
            && followsOnRow(glyphs.back().area, *area)) {
            glyphs.back().text += text;
            glyphs.back().area.right = std::max(glyphs.back().area.right, area->right);
            glyphs.back().endOfLine = box.endOfLine;
        } else {
            glyphs.push_back({ std::move(text), *area, box.endOfLine });
        }
    }

    const auto order = orderWordRows(glyphs);
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        auto& glyph = glyphs[order.empty() ? index : order[index]];
        const TextGlyph* next = nullptr;
        if (index + 1 < glyphs.size())
            next = &glyphs[order.empty() ? index + 1 : order[index + 1]];
        const bool sameRow = next && followsOnRow(glyph.area, next->area);
        // MuPDF supplies word separators. Make consecutive glyph advances meet,
        // including overlaps, so Okular does not invent additional separators.
        if (sameRow && !glyph.endOfLine)
            glyph.area.right = next->area.left;
        if (glyph.endOfLine)
            glyph.text += QLatin1Char('\n');
        result->append(glyph.text, glyph.area);
        // TextPage owns its string; release our temporary glyph buffer.
        glyph.text.clear();

        // Adjacent source lines can be cells in the same table row. A separate
        // whitespace box joins their layout regions without widening a glyph
        // across the column gap or letting XY-cut move the column to the footer.
        if (sameRow && glyph.endOfLine && next->area.left > glyph.area.right) {
            result->append(
                QStringLiteral("\t"),
                Okular::NormalizedRect(glyph.area.right, glyph.area.top, next->area.left, glyph.area.bottom));
        }
    }
    return result.release();
}

Okular::TextPage* mergedTextPage(const std::vector<Model::TextBox>& nativeBoxes,
                                 qreal width,
                                 qreal height,
                                 const QVector<Plugin::Caching::OCR::CacheItem>& additions)
{
    std::unique_ptr<Okular::TextPage> result(textPage(nativeBoxes, width, height));
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
        return result.release();
    for (const auto& box : additions) {
        if (box.ch.isEmpty())
            continue;
        const auto area = normalizeTextArea(box.l, box.t, box.r, box.b, 1.0, 1.0);
        if (!area)
            continue;
        result->append(box.ch, *area);
    }
    // Page::setTextPage applies Okular's geometric reading-order analysis.
    return result.release();
}

QString plainText(const std::vector<Model::TextBox>& boxes)
{
    QString text;
    for (const auto& box : boxes) {
        text += QString::fromStdString(box.text);
        if (box.endOfLine)
            text += QLatin1Char('\n');
    }
    return text;
}

QString captureReflowText(const std::vector<Model::TextBox>& boxes, double top)
{
    QString text;
    const auto first = std::find_if(boxes.begin(), boxes.end(), [top](const auto& box) { return box.bottom > top; });
    for (auto it = first; it != boxes.end() && text.size() < 64; ++it)
        text += normalizeReflowText(QString::fromStdString(it->text));
    return text.size() >= 24 ? text.left(64) : QString();
}

std::optional<double> findReflowText(const std::vector<Model::TextBox>& boxes, const QString& snippet)
{
    if (snippet.isEmpty())
        return std::nullopt;
    const auto text = normalizeReflowText(plainText(boxes));
    auto offset = text.indexOf(snippet);
    if (offset < 0 || text.indexOf(snippet, offset + 1) >= 0)
        return std::nullopt;
    for (const auto& box : boxes) {
        const auto length = normalizeReflowText(QString::fromStdString(box.text)).size();
        if (offset < length)
            return std::max(0.0, box.top);
        offset -= length;
    }
    return std::nullopt;
}

} // namespace Mu::Generator::Conversion
