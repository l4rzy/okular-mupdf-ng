// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_TEMP_DIR_HPP
#define MU_PLUGIN_UTIL_TEMP_DIR_HPP

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace Mu::Plugin::Util {

/**
 * Returns the path to the per-user temporary directory for okular-mupdf-ng.
 * Creates <temp>/okular-mupdf-ng-<uid> with 0700 permissions and returns an
 * empty string when the directory cannot be created or locked down.
 */
inline QString tempDirectory()
{
    // The per-user name means two accounts can never share a directory, so one
    // user's 0700 directory cannot block another user's worker sockets or be
    // reached by stale-file cleanup.
    const QString path = QDir::tempPath() + QStringLiteral("/okular-mupdf-ng-%1").arg(::getuid());
    if (!QDir().mkpath(path)) {
        qWarning() << "okular-mupdf-ng: failed to create temp directory" << path;
        return { };
    }
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        qWarning() << "okular-mupdf-ng: failed to lock down temp directory" << path;
        return { };
    }
    return path;
}

/**
 * Cleans up orphan socket files left behind by crashed plugin processes.
 * Called from the plugin side on worker start.
 */
inline void cleanupStaleTempFiles()
{
    // Only remove worker socket names whose encoded owner PID is definitely
    // gone; an active worker's files must survive a later plugin start.
    const QString path = tempDirectory();
    if (path.isEmpty())
        return;
    QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::System | QDir::Hidden, QDir::Unsorted);

    for (const QFileInfo& info : entries) {
        const QString name = info.fileName();
        if (!name.startsWith(QStringLiteral("worker-")))
            continue;
        // Format: worker-<PID>-<UUID>-{ctrl,fd}.sock
        const qsizetype firstDash = name.indexOf(u'-');
        const qsizetype secondDash = name.indexOf(u'-', firstDash + 1);
        if (firstDash == -1 || secondDash == -1)
            continue;
        bool ok = false;
        const pid_t pid = static_cast<pid_t>(name.mid(firstDash + 1, secondDash - firstDash - 1).toLongLong(&ok));
        if (ok && pid > 0 && ::kill(pid, 0) == -1 && errno == ESRCH)
            QFile::remove(info.absoluteFilePath());
    }
}

} // namespace Mu::Plugin::Util

#endif // MU_PLUGIN_UTIL_TEMP_DIR_HPP
