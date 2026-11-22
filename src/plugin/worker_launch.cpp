// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_launch.hpp"

#include "shared/logging.hpp"
#include "shared/transport/poll.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
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

qint64 startBubblewrap(QProcess& process,
                       const QString& launcher,
                       const QString& worker,
                       QStringList arguments,
                       const QString& socketDirectory,
                       const QStringList& tessDataDirectories,
                       int timeoutMs)
{
    QTemporaryFile statusFile;
    if (!statusFile.open())
        return 0;
    // The launcher writes through its own descriptor while we poll this reader.
    QFile statusReader(statusFile.fileName());
    if (!statusReader.open(QIODevice::ReadOnly))
        return 0;
    QStringList readOnlyPaths;
    QStringList candidates { QStringLiteral("/usr"),
                             QStringLiteral("/lib"),
                             QStringLiteral("/lib64"),
                             QStringLiteral("/etc/ld.so.cache"),
                             QStringLiteral(MU_BWRAP_TESSDATA_DIR),
                             QStringLiteral(MU_BWRAP_SIGNATURE_FONT_DIR) };
    candidates.append(tessDataDirectories);
    for (const auto& candidate : candidates) {
        if (candidate.isEmpty())
            continue;
        const QString path = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
        if (QFileInfo::exists(path) && !readOnlyPaths.contains(path))
            readOnlyPaths.append(path);
    }
    arguments << QStringLiteral("--parent-pid") << QString::number(QCoreApplication::applicationPid())
              << QStringLiteral("--bwrap");
    const int statusFd = statusFile.handle();
    process.setChildProcessModifier([&process, statusFd] {
        if (::fcntl(statusFd, F_SETFD, 0) < 0)
            process.failChildProcessModifier("preserve Bubblewrap status descriptor", errno);
    });
    process.start(launcher, buildBubblewrapArguments(worker, arguments, socketDirectory, readOnlyPaths, statusFd));
    const bool started = process.waitForStarted(timeoutMs);
    process.setChildProcessModifier({ });
    if (!started) {
        MU_LOG(warning, "Mu::Plugin", "Bubblewrap launch failed: " + process.errorString().toStdString());
        return 0;
    }
    // Poll quickly during normal startup and back off if the launcher stalls.
    constexpr int MaxStatusRetryMs = 10;
    const auto deadline = IPC::MonotonicDeadline::fromMilliseconds(timeoutMs);
    for (int retryMs = 1; !deadline.expired(); retryMs = std::min(retryMs * 2, MaxStatusRetryMs)) {
        statusReader.seek(0);
        if (const auto childPid = parseBubblewrapPid(statusReader.read(4096)))
            return *childPid;
        if (process.state() == QProcess::NotRunning)
            break;
        process.waitForFinished(std::min(retryMs, deadline.pollTimeoutMilliseconds()));
    }
    MU_LOG(warning, "Mu::Plugin", "Bubblewrap did not report a worker PID");
    return 0;
}

} // namespace Mu::Plugin
