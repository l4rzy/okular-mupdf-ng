// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/vacuum.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "plugin/caching/cache_file.hpp"

namespace Mu::Plugin::Caching::Vacuum {

namespace {

const std::array<std::pair<CacheKind, QString>, 3> KnownSubdirectories {
    std::pair { CacheKind::Ocr, QStringLiteral("ocr") },
    std::pair { CacheKind::EpubAccelerator, QStringLiteral("epub_accel") },
    std::pair { CacheKind::PdfToc, QStringLiteral("pdf_toc") }
};

bool canScanDirectory(const QFileInfo& info)
{
    // Listing needs read permission; inspecting children also needs search
    // (execute) permission. Without both, a partial scan could look empty.
    return info.isDir() && info.isReadable() && info.isExecutable();
}

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

CacheUsage measureCacheUsage(const QString& root)
{
    CacheUsage result;
    const QString cacheRoot = QDir::cleanPath(root.isEmpty() ? directory(QString()) : root);
    const QFileInfo rootInfo(cacheRoot);
    if (!rootInfo.exists() || rootInfo.isSymLink())
        return result;
    if (!canScanDirectory(rootInfo)) {
        ++result.errors;
        return result;
    }
    for (const auto& [kind, name] : KnownSubdirectories) {
        const QString base = cacheRoot + QLatin1Char('/') + name;
        const QFileInfo baseInfo(base);
        if (!baseInfo.exists() || baseInfo.isSymLink())
            continue;
        if (!canScanDirectory(baseInfo)) {
            ++result.errors;
            continue;
        }
        qint64 bytes = 0;
        QDirIterator files(
            base, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const QFileInfo info = files.fileInfo();
            if (info.isSymLink())
                continue;
            if (info.isFile())
                bytes += info.size();
            else if (info.isDir() && !canScanDirectory(info))
                ++result.errors;
        }
        if (kind == CacheKind::Ocr)
            result.ocrBytes = bytes;
        else if (kind == CacheKind::EpubAccelerator)
            result.epubAcceleratorBytes = bytes;
        else
            result.pdfTocBytes = bytes;
    }
    return result;
}

namespace {

VacuumResult removeCacheFiles(const QString& root, const std::optional<QDateTime>& now, CacheKind kind)
{
    VacuumResult result;
    const QString cacheRoot = QDir::cleanPath(root.isEmpty() ? directory(QString()) : root);
    const QFileInfo rootInfo(cacheRoot);
    if ((now && !now->isValid()) || !rootInfo.exists() || rootInfo.isSymLink())
        return result;
    if (!canScanDirectory(rootInfo)) {
        ++result.errors;
        return result;
    }

    for (const auto& [cacheKind, name] : KnownSubdirectories) {
        if (kind != CacheKind::All && kind != cacheKind)
            continue;
        const QString base = cacheRoot + QLatin1Char('/') + name;
        const QFileInfo baseInfo(base);
        if (!baseInfo.exists() || baseInfo.isSymLink())
            continue;
        if (!canScanDirectory(baseInfo)) {
            ++result.errors;
            continue;
        }

        std::vector<QString> dirs;
        QDirIterator files(
            base, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const QFileInfo info = files.fileInfo();
            // Never follow or delete symlinks; remove only regular cache files.
            if (info.isSymLink())
                continue;
            if (info.isDir()) {
                if (canScanDirectory(info))
                    dirs.push_back(info.absoluteFilePath());
                else
                    ++result.errors;
                continue;
            }
            if (!info.isFile() || (now && !isStale(info.lastModified(), *now)))
                continue;
            if (QFile::remove(info.absoluteFilePath()))
                ++result.filesRemoved;
            else if (QFileInfo::exists(info.absoluteFilePath()))
                ++result.errors;
        }

        // Prune directories left empty, deepest first. The base itself is
        // kept so later saves never observe a missing tree.
        std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
        for (const QString& dir : dirs) {
            if (QDir(dir).isEmpty() && QDir().rmdir(dir))
                ++result.dirsRemoved;
        }
    }
    return result;
}

} // namespace

VacuumResult clearCaches(const QString& root, CacheKind kind)
{
    return removeCacheFiles(root, std::nullopt, kind);
}

VacuumResult vacuumStaleCaches(const QString& root, const QDateTime& now, CacheKind kind)
{
    return removeCacheFiles(root, now, kind);
}

VacuumResult vacuumStaleCaches(const QDateTime& now)
{
    return vacuumStaleCaches(QString { }, now);
}

} // namespace Mu::Plugin::Caching::Vacuum
