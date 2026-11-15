// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef MU_ENABLE_WORKER_BWRAP
#include <QByteArray>
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

} // namespace Mu::Plugin
#endif // MU_ENABLE_WORKER_BWRAP
