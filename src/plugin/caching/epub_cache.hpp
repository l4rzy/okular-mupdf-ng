// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CACHING_EPUB_CACHE_HPP
#define MU_PLUGIN_CACHING_EPUB_CACHE_HPP

#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Caching::EPUB {

/// Cache data that may be persisted independently as it becomes available.
struct CacheEntry {
    /// MuPDF's EPUB layout accelerator, when one has been generated.
    std::optional<QByteArray> accelerator;
    /// Parsed document outline, including an intentionally valid empty outline.
    std::optional<std::vector<Model::OutlineNode>> outline;
};

/// Settings-aware persistent cache for EPUB layout and outline data.
class Cache {
public:
    /// Loads a cache entry when the source identity and layout settings match.
    [[nodiscard]] static std::optional<CacheEntry> load(const QString& path, const Model::DocumentSettings& settings);

    /// Stores or replaces the accelerator while preserving any cached outline.
    [[nodiscard]] static bool
    saveAccelerator(const QString& path, const Model::DocumentSettings& settings, const QByteArray& bytes);

    /// Stores or replaces the outline while preserving any cached accelerator.
    [[nodiscard]] static bool saveOutline(const QString& path,
                                          const Model::DocumentSettings& settings,
                                          const std::vector<Model::OutlineNode>& outline);

    /// Settings-aware stable cache filename for the canonical source.
    [[nodiscard]] static QString cacheFilePath(const QString& path, const Model::DocumentSettings& settings);

    /// The EPUB layout fingerprint that participates in the cache filename.
    /// Callers that memoize the cache path compare this key instead of the
    /// whole settings struct.
    [[nodiscard]] static QByteArray layoutKey(const Model::DocumentSettings& settings);

    /// Path-keyed variants for callers that memoize the derived cache path.
    [[nodiscard]] static std::optional<CacheEntry> loadAt(const QString& cachePath);
    [[nodiscard]] static bool saveAcceleratorAt(const QString& cachePath, const QByteArray& bytes);
    [[nodiscard]] static bool saveOutlineAt(const QString& cachePath, const std::vector<Model::OutlineNode>& outline);
};

} // namespace Mu::Plugin::Caching::EPUB

#endif // MU_PLUGIN_CACHING_EPUB_CACHE_HPP
