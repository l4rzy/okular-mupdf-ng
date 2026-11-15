// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_launch.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <limits>

namespace Mu::Plugin {

QStringList buildBubblewrapArguments(const QString& worker,
                                     const QStringList& workerArguments,
                                     const QString& socketDirectory,
                                     const QStringList& readOnlyPaths,
                                     int statusFd)
{
    // Keep the host PID namespace so SO_PEERCRED identifies both IPC peers.
    QStringList arguments { QStringLiteral("--unshare-user"),
                            QStringLiteral("--unshare-net"),
                            QStringLiteral("--unshare-ipc"),
                            QStringLiteral("--unshare-uts"),
                            QStringLiteral("--die-with-parent"),
                            QStringLiteral("--new-session"),
                            QStringLiteral("--cap-drop"),
                            QStringLiteral("ALL"),
                            QStringLiteral("--clearenv"),
                            QStringLiteral("--setenv"),
                            QStringLiteral("PATH"),
                            QStringLiteral("/usr/bin"),
                            QStringLiteral("--setenv"),
                            QStringLiteral("LANG"),
                            QStringLiteral("C.UTF-8"),
                            QStringLiteral("--setenv"),
                            QStringLiteral("HOME"),
                            QStringLiteral("/nonexistent"),
                            QStringLiteral("--setenv"),
                            QStringLiteral("TMPDIR"),
                            QStringLiteral("/tmp"),
                            QStringLiteral("--proc"),
                            QStringLiteral("/proc"),
                            QStringLiteral("--dev"),
                            QStringLiteral("/dev"),
                            QStringLiteral("--tmpfs"),
                            QStringLiteral("/tmp"),
                            QStringLiteral("--chdir"),
                            QStringLiteral("/"),
                            QStringLiteral("--info-fd"),
                            QString::number(statusFd) };
    for (const auto& path : readOnlyPaths)
        arguments << QStringLiteral("--ro-bind") << path << path;
    arguments << QStringLiteral("--ro-bind") << worker << worker;
    arguments << QStringLiteral("--bind") << socketDirectory << socketDirectory;
    arguments << QStringLiteral("--") << worker;
    arguments.append(workerArguments);
    return arguments;
}

std::optional<qint64> parseBubblewrapPid(const QByteArray& status)
{
    const auto value = QJsonDocument::fromJson(status).object().value(QStringLiteral("child-pid"));
    const auto pid = value.toInteger(-1);
    if (!value.isDouble() || pid <= 0 || pid > std::numeric_limits<int>::max()
        || value.toDouble() != static_cast<double>(pid))
        return std::nullopt;
    return pid;
}

} // namespace Mu::Plugin
