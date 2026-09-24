// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CACHING_VACUUM_HPP
#define MU_PLUGIN_CACHING_VACUUM_HPP

#include <QDateTime>
#include <QString>

namespace Mu::Plugin::Caching::Vacuum {

// Files whose mtime is older than this are removed by the vacuum.
constexpr int StaleDays = 90;
// Minimum interval between two vacuum runs, gated by the stored timestamp.
constexpr int ThrottleDays = 7;

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
};

/// Removes stale files from ocr_cache and epub_accelerators under the
/// production cache root. Missing trees are a no-op; symlinks are skipped.
[[nodiscard]] VacuumResult vacuumStaleCaches(const QDateTime& now);
/// Test seam: same vacuum scoped to an explicit root directory.
[[nodiscard]] VacuumResult vacuumStaleCaches(const QString& root, const QDateTime& now);

} // namespace Mu::Plugin::Caching::Vacuum

#endif // MU_PLUGIN_CACHING_VACUUM_HPP
