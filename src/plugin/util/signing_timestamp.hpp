// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_SIGNING_TIMESTAMP_HPP
#define MU_PLUGIN_UTIL_SIGNING_TIMESTAMP_HPP

#include <QDateTime>
#include <QString>

namespace Mu::Plugin::Util::SigningTimestamp {

/// Signing timestamp shared with the worker so the PDF /M dictionary entry and
/// the signature appearance text always describe the same instant.
struct Timestamp {
    qint64 epochSeconds = 0;
    QString displayDate;
};

/**
 * Formats a timestamp as a compact user-facing signature date,
 * e.g. "Sep 7, 2026 13:14 CDT", using the timezone of @p when
 * and the C locale (English abbreviations, matching MuPDF's English box labels).
 */
[[nodiscard]] QString displayDate(const QDateTime& when);

/**
 * Captures the current instant for a signing operation.
 */
[[nodiscard]] Timestamp current();

} // namespace Mu::Plugin::Util::SigningTimestamp

#endif // MU_PLUGIN_UTIL_SIGNING_TIMESTAMP_HPP
