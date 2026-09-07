// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_SIGNATURE_DATE_HPP
#define MU_WORKER_ENGINE_SIGNATURE_DATE_HPP

#include <cstdint>
#include <ctime>
#include <string>

namespace Mu::Worker::Engine {

/// Formats an epoch timestamp as a compact user-facing signature date,
/// e.g. "Sep 7, 2026 13:14 CDT", using the process local timezone.
/// Fallback for requests without a plugin-provided display date.
inline std::string formatSignatureDate(std::int64_t epochSeconds)
{
    const std::time_t time = static_cast<std::time_t>(epochSeconds);
    std::tm broken { };
    ::localtime_r(&time, &broken);
    char formatted[64] { };
    const std::size_t length = std::strftime(formatted, sizeof formatted, "%b %d, %Y %H:%M %Z", &broken);
    if (length == 0)
        return { };
    std::string result(formatted, length);
    // Strip the leading zero of single-digit days: "Sep 07" -> "Sep 7".
    if (result.size() > 4 && result[3] == ' ' && result[4] == '0')
        result.erase(4, 1);
    return result;
}

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_SIGNATURE_DATE_HPP
