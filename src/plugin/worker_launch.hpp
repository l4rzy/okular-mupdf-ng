// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QProcess>
#include <QStringList>
#include <optional>

namespace Mu::Plugin {

// Paths must be absolute. The caller selects existing read-only runtime paths.
QStringList buildBubblewrapArguments(const QString& worker,
                                     const QStringList& workerArguments,
                                     const QString& socketDirectory,
                                     const QStringList& readOnlyPaths,
                                     int statusFd);

// Bubblewrap reports the child's host PID, independently of the worker's IPC.
std::optional<qint64> parseBubblewrapPid(const QByteArray& status);

// Returns the worker's host PID or zero on failure. The caller owns process cleanup.
qint64 startBubblewrap(QProcess& process,
                       const QString& launcher,
                       const QString& worker,
                       QStringList arguments,
                       const QString& socketDirectory,
                       const QStringList& tessDataDirectories,
                       int timeoutMs);

} // namespace Mu::Plugin
