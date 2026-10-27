// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_OCR_OPTIONS_HPP
#define MU_PLUGIN_UTIL_OCR_OPTIONS_HPP

#include <QLatin1String>
#include <QString>

namespace Mu::Plugin::Util {

/// Removes the `.traineddata` suffix from a language identifier.
inline QString stripLangSuffix(const QString& lang)
{
    constexpr auto suffix = QLatin1String(".traineddata");
    QString clean = lang;
    if (clean.endsWith(suffix))
        clean.chop(suffix.size());
    return clean;
}

/// Maps the configured OCR quality level to its target DPI. Unknown values use
/// the balanced default so they do not accidentally request the slowest mode.
float qualityToDpi(int quality);

} // namespace Mu::Plugin::Util

#endif // MU_PLUGIN_UTIL_OCR_OPTIONS_HPP
