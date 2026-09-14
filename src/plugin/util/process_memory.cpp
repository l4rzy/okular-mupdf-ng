// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/process_memory.hpp"

#include <limits>

#include <QFile>
#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

namespace Mu::Plugin::Util {

std::optional<quint64> processResidentBytes(qint64 pid)
{
#ifdef Q_OS_LINUX
    if (pid <= 0)
        return std::nullopt;
    QFile status(QStringLiteral("/proc/%1/statm").arg(pid));
    if (!status.open(QIODevice::ReadOnly | QIODevice::Text))
        return std::nullopt;
    // statm is a single short line: size resident shared text lib data dt.
    const QList<QByteArray> fields = status.readAll().trimmed().split(' ');
    if (fields.size() < 2)
        return std::nullopt;
    bool ok = false;
    const quint64 resident = QString::fromLatin1(fields.at(1)).toULongLong(&ok);
    if (!ok)
        return std::nullopt;
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || resident > std::numeric_limits<quint64>::max() / static_cast<quint64>(pageSize))
        return std::nullopt;
    return resident * static_cast<quint64>(pageSize);
#else
    Q_UNUSED(pid);
    return std::nullopt;
#endif
}

} // namespace Mu::Plugin::Util
