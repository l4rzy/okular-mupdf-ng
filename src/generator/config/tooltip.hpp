// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_TOOLTIP_HPP
#define MU_GENERATOR_CONFIG_TOOLTIP_HPP

#include <QString>
#include <QStringList>

#include <string>

namespace Mu::Generator::Config {

// Wrap after translation, counting Unicode code points rather than UTF-16 units.
// Preserve explicit line breaks, including empty lines between paragraphs.
inline QString wrapToolTip(const QString& text)
{
    constexpr std::size_t lineLimit = 80;
    QStringList lines;
    for (const QString& paragraph : text.split(QLatin1Char('\n'))) {
        const std::u32string points = paragraph.trimmed().toStdU32String();
        std::size_t start = 0;
        while (points.size() - start > lineLimit) {
            std::size_t end = start + lineLimit;
            while (end > start && !QChar::isSpace(points[end]))
                --end;
            // An unbroken word longer than the limit must be split.
            if (end == start)
                end = start + lineLimit;
            lines.append(QString::fromStdU32String(points.substr(start, end - start)).trimmed());
            start = end;
            while (start < points.size() && QChar::isSpace(points[start]))
                ++start;
        }
        if (start < points.size() || points.empty())
            lines.append(QString::fromStdU32String(points.substr(start)));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace Mu::Generator::Config

#endif // MU_GENERATOR_CONFIG_TOOLTIP_HPP
