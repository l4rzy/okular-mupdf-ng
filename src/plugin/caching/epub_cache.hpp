// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CACHING_EPUB_CACHE_HPP
#define MUPDF_PLUGIN_CACHING_EPUB_CACHE_HPP

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

private:
    /// Derives a stable cache filename from the canonical source and EPUB layout.
    [[nodiscard]] static QString cacheFilePath(const QString& path, const Model::DocumentSettings& settings);
};

} // namespace Mu::Plugin::Caching::EPUB

#endif // MUPDF_PLUGIN_CACHING_EPUB_CACHE_HPP
