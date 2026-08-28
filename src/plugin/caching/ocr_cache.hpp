// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CACHING_OCR_CACHE_HPP
#define MUPDF_PLUGIN_CACHING_OCR_CACHE_HPP

#include <optional>
#include <vector>

#include <QString>
#include <QVector>

#include "plugin/caching/ocr_constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Plugin::Caching::OCR {

/// Normalized on-disk identity for one document, language, and OCR resolution.
/// Keeping language and DPI in the key prevents cross-configuration reuse.
struct CacheKey {
    /// Sanitized document content hash used as the cache directory name.
    QString documentHash;
    /// Lowercase language identifier without a traineddata or DPI suffix.
    QString language;
    /// Requested resolution, or zero when no explicit DPI was supplied.
    int dpi = 0;

    bool operator==(const CacheKey& other) const = default;
};

/// One OCR text box in the cache's Qt-friendly representation.
struct CacheItem {
    /// UTF-8 text recognized in the box.
    QString ch;
    /// Normalized page bounds in the inclusive [0, 1] range.
    double l, t, r, b;
};

/// Result of a cache lookup, distinguishing an empty hit from a miss.
struct CacheLoadResult {
    /// True when a valid cache file was found, even if it contains no boxes.
    bool present = false;
    /// Cached OCR boxes; empty is meaningful when present is true.
    QVector<CacheItem> items;
};

/// Persistent, DPI-aware cache for page-level OCR results.
class Cache {
public:
    /// Normalizes document, language, and DPI spellings into one cache key.
    static std::optional<CacheKey> normalizeKey(const QString& docHash, const QString& lang, int dpi = 0);

    /// Returns the cache path for a normalized key and non-negative page index.
    static QString getCacheFilePath(const CacheKey& key, int pageNum);

    /// Loads a page result, allowing configured higher-DPI fallback entries.
    static CacheLoadResult load(const CacheKey& key, int pageNum);

    /// Saves a page result atomically after enforcing cache limits.
    static bool save(const CacheKey& key, int pageNum, const QVector<CacheItem>& items);

    /// Normalizes raw arguments and returns their corresponding cache path.
    static QString getCacheFilePath(const QString& docHash, int pageNum, const QString& lang, int dpi = 0);

    /// Normalizes raw arguments and loads the page result.
    static CacheLoadResult load(const QString& docHash, int pageNum, const QString& lang, int dpi = 0);

    /// Normalizes raw arguments and saves the page result.
    static bool
    save(const QString& docHash, int pageNum, const QString& lang, int dpi, const QVector<CacheItem>& items);

    /// Saves a result with no explicit DPI in its cache key.
    static bool save(const QString& docHash, int pageNum, const QString& lang, const QVector<CacheItem>& items)
    {
        return save(docHash, pageNum, lang, 0, items);
    }

    /// Removes the `.traineddata` suffix from a language identifier.
    static QString stripLangSuffix(const QString& lang);

    /// Maps the configured OCR quality level to its target DPI.
    static float qualityToDpi(int quality);

    /// Converts worker text boxes into the cache representation.
    static QVector<CacheItem> convertToCacheItems(const std::vector<Model::TextBox>& boxes);
};

} // namespace Mu::Plugin::Caching::OCR

#endif // MUPDF_PLUGIN_CACHING_OCR_CACHE_HPP
