// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_PDF_FORMAT_HINT_HPP
#define MU_WORKER_ENGINE_PDF_FORMAT_HINT_HPP

#include <array>
#include <charconv>
#include <string_view>

#include "shared/protocol/limits.hpp"

namespace Mu::Worker::Engine {

// Recognize standalone Acrobat date/time calls with literal arguments only.
// Indexed formats match the tables in MuPDF's source/pdf/js/util.js.
inline std::string_view parseFormatHint(std::string_view script)
{
    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return std::string_view { };
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    script = trim(script);
    if (script.ends_with(';'))
        script = trim(script.substr(0, script.size() - 1));
    const auto open = script.find('(');
    if (open == std::string_view::npos || !script.ends_with(')'))
        return { };
    const auto function = trim(script.substr(0, open));
    const auto argument = trim(script.substr(open + 1, script.size() - open - 2));
    if (function == "AFDate_FormatEx" || function == "AFTime_FormatEx") {
        if (argument.size() < 2 || (argument.front() != '\'' && argument.front() != '"')
            || argument.back() != argument.front())
            return { };
        const auto format = argument.substr(1, argument.size() - 2);
        if (format.empty() || format.size() > Limit::MaxFormFormatHintBytes)
            return { };
        for (const char value : format) {
            const auto c = static_cast<unsigned char>(value);
            if (c < 32 || c > 126 || c == '\\' || value == argument.front())
                return { };
        }
        return format;
    }
    if (function != "AFDate_Format" && function != "AFTime_Format")
        return { };
    // Ignore alternate numeric spellings and expressions rather than guessing
    // JavaScript coercion or legacy octal semantics.
    if (argument.empty() || (argument.size() > 1 && argument.front() == '0'))
        return { };
    unsigned index = 0;
    const auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), index);
    if (error != std::errc() || end != argument.data() + argument.size())
        return { };
    static constexpr std::array<std::string_view, 14> dateFormats {
        "m/d",      "m/d/yy", "mm/dd/yy", "mm/yy",       "d-mmm",        "d-mmm-yy",       "dd-mm-yy",
        "yy-mm-dd", "mmm-yy", "mmmm-yy",  "mmm d, yyyy", "mmmm d, yyyy", "m/d/yy h:MM tt", "m/d/yy HH:MM"
    };
    static constexpr std::array<std::string_view, 4> timeFormats { "HH:MM", "h:MM tt", "HH:MM:ss", "h:MM:ss tt" };
    if (function == "AFDate_Format")
        return index < dateFormats.size() ? dateFormats[index] : std::string_view { };
    return index < timeFormats.size() ? timeFormats[index] : std::string_view { };
}

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_PDF_FORMAT_HINT_HPP
