// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/ocr_options.hpp"
#include <paperbox/ocr.hpp>

namespace Mu::Plugin::Util {

QString normalizeOcrLanguage(const QString& language, int& dpi)
{
    QString normalized;
    normalized.reserve(language.size());
    for (const QChar ch : stripLangSuffix(language).toLower()) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_') || ch == QLatin1Char('.'))
            normalized.append(ch);
        else if (!normalized.isEmpty() && !normalized.endsWith(QLatin1Char('_')))
            normalized.append(QLatin1Char('_'));
    }
    const auto isSeparator = [](QChar ch) {
        return ch == QLatin1Char('_') || ch == QLatin1Char('.');
    };
    while (!normalized.isEmpty() && isSeparator(normalized.back()))
        normalized.chop(1);
    qsizetype start = 0;
    while (start < normalized.size() && isSeparator(normalized.at(start)))
        ++start;
    normalized.remove(0, start);

    const auto underscore = normalized.lastIndexOf(QLatin1Char('_'));
    if (underscore > 0 && normalized.endsWith(QLatin1String("dpi"))) {
        bool hasDpi = false;
        const int suffixDpi =
            QStringView(normalized).mid(underscore + 1, normalized.size() - underscore - 4).toInt(&hasDpi);
        if (hasDpi) {
            normalized.truncate(underscore);
            if (dpi <= 0)
                dpi = suffixDpi;
        }
    }
    return normalized;
}

float qualityToDpi(int quality)
{
    switch (quality) {
    case 0: // Speed
        return paperbox::ocr::FastDpi;
    case 2: // Accuracy
        return paperbox::ocr::AccuracyDpi;
    default:
        return paperbox::ocr::BalancedDpi;
    }
}

} // namespace Mu::Plugin::Util
