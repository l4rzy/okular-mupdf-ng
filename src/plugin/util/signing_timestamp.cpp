// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/signing_timestamp.hpp"

#include <QLocale>

namespace Mu::Plugin::Util::SigningTimestamp {

QString displayDate(const QDateTime& when)
{
    return QLocale::c().toString(when, QStringLiteral("MMM d, yyyy HH:mm t"));
}

Timestamp current(bool useUtc)
{
    const QDateTime now = useUtc ? QDateTime::currentDateTimeUtc() : QDateTime::currentDateTime();
    return { now.toSecsSinceEpoch(), displayDate(now) };
}

} // namespace Mu::Plugin::Util::SigningTimestamp
