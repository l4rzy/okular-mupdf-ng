// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/vacuum.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <vector>

#include "plugin/caching/cache_file.hpp"

namespace Mu::Plugin::Caching::Vacuum {

namespace {

const std::array<QString, 2> KnownSubdirectories { QStringLiteral("ocr_cache"), QStringLiteral("epub_accelerators") };

} // namespace

QDateTime parseLastVacuum(const QString& value)
{
    if (value.isEmpty())
        return { };
    // ISO-8601 UTC as written by formatLastVacuum; fall back to a
    // timezone-less parse so older hand-edited values still decode.
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed;
}

QString formatLastVacuum(const QDateTime& time)
{
    return time.toUTC().toString(Qt::ISODate);
}

bool shouldVacuum(const QDateTime& last, const QDateTime& now)
{
    // No record, or a future record from clock skew: run once and repair
    // the stored timestamp afterwards.
    if (!last.isValid() || last > now)
        return true;
    return last.addDays(ThrottleDays) <= now;
}

bool isStale(const QDateTime& modified, const QDateTime& now)
{
    // Never delete on unknown or future mtimes; only clear age-out.
    if (!modified.isValid() || modified > now)
        return false;
    return modified.addDays(StaleDays) <= now;
}

VacuumResult vacuumStaleCaches(const QString& root, const QDateTime& now)
{
    VacuumResult result;
    if (!now.isValid())
        return result;

    for (const auto& name : KnownSubdirectories) {
        const QString base = root.isEmpty() ? directory(name) : root + QLatin1Char('/') + name;
        QDir baseDir(base);
        if (!baseDir.exists())
            continue;

        QDirIterator files(base, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const QFileInfo info = files.fileInfo();
            // Never follow or delete symlinks; only regular cache files age out.
            if (info.isSymLink())
                continue;
            if (!isStale(info.lastModified(), now))
                continue;
            if (QFile::remove(info.absoluteFilePath()))
                ++result.filesRemoved;
        }

        // Prune directories left empty, deepest first. The base itself is
        // kept so later saves never observe a missing tree.
        std::vector<QString> dirs;
        QDirIterator it(base, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            if (it.fileInfo().isSymLink())
                continue;
            dirs.push_back(it.filePath());
        }
        std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
        for (const QString& dir : dirs) {
            if (QDir(dir).isEmpty() && QDir().rmdir(dir))
                ++result.dirsRemoved;
        }
    }
    return result;
}

VacuumResult vacuumStaleCaches(const QDateTime& now)
{
    return vacuumStaleCaches(QString { }, now);
}

} // namespace Mu::Plugin::Caching::Vacuum
