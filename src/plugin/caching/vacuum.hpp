// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CACHING_VACUUM_HPP
#define MU_PLUGIN_CACHING_VACUUM_HPP

#include <QDateTime>
#include <QString>

namespace Mu::Plugin::Caching::Vacuum {

// Files whose mtime is older than this are removed by automatic vacuuming.
constexpr int StaleDays = 90;
// Minimum interval between two vacuum runs, gated by the stored timestamp.
constexpr int ThrottleDays = 7;

enum class CacheKind { All, Ocr, EpubAccelerator, PdfToc };

/// Parses the hidden CacheLastVacuum config value; invalid when empty or malformed.
[[nodiscard]] QDateTime parseLastVacuum(const QString& value);
/// Formats a timestamp for the hidden CacheLastVacuum config value (UTC, ISO-8601).
[[nodiscard]] QString formatLastVacuum(const QDateTime& time);
/// Pure throttle decision: true when no valid timestamp exists, the stored
/// time lies in the future (clock skew), or it is at least ThrottleDays old.
[[nodiscard]] bool shouldVacuum(const QDateTime& last, const QDateTime& now);
/// Pure staleness decision on file mtime; invalid or future mtimes are never stale.
[[nodiscard]] bool isStale(const QDateTime& modified, const QDateTime& now);

struct VacuumResult {
    int filesRemoved = 0;
    int dirsRemoved = 0;
    int errors = 0;
};

struct CacheUsage {
    qint64 ocrBytes = 0;
    qint64 epubAcceleratorBytes = 0;
    qint64 pdfTocBytes = 0;
    int errors = 0;

    [[nodiscard]] qint64 totalBytes() const noexcept { return ocrBytes + epubAcceleratorBytes + pdfTocBytes; }
};

/// Totals regular file sizes in the same trees as vacuum; skips symlinks.
/// An empty root selects the production cache root.
[[nodiscard]] CacheUsage measureCacheUsage(const QString& root = { });

/// Removes all regular files from the selected cache trees, regardless of age.
/// An empty root selects the production cache root; symlinks are skipped.
[[nodiscard]] VacuumResult clearCaches(const QString& root = { }, CacheKind kind = CacheKind::All);

/// Removes stale files from ocr, epub_accel, and pdf_toc under the
/// production cache root. Missing trees are a no-op; symlinks are skipped.
[[nodiscard]] VacuumResult vacuumStaleCaches(const QDateTime& now);
/// Removes stale files from the selected cache trees; an empty root selects
/// the production cache root. The default kind cleans all known trees.
[[nodiscard]] VacuumResult
vacuumStaleCaches(const QString& root, const QDateTime& now, CacheKind kind = CacheKind::All);

} // namespace Mu::Plugin::Caching::Vacuum

#endif // MU_PLUGIN_CACHING_VACUUM_HPP
