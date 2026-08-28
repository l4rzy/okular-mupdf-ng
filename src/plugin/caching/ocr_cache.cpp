// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/ocr_cache.hpp"

#include "plugin/caching/cache_file.hpp"

#include <QDataStream>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <optional>
#include <zlib.h>

#include "plugin/caching/ocr_constants.hpp"
#include "shared/logging.hpp"

namespace Mu::Plugin::Caching::OCR {

namespace OcrConstant = ::Mu::Plugin::OCR::Constant;

inline bool isCacheFileSizeValid(qint64 size)
{
    // An empty result has only the fixed header; non-empty results must also
    // fit inside the compressed payload limit.
    return size == OcrConstant::CACHE_HEADER_BYTES
        || (size > OcrConstant::CACHE_HEADER_BYTES
            && size <= (OcrConstant::CACHE_HEADER_BYTES + OcrConstant::MAX_CACHE_COMPRESSED_BYTES));
}

inline bool isValidCacheItem(const CacheItem& item)
{
    // Cached coordinates are consumed as normalized rectangles by the text
    // conversion layer, so reject non-finite or inverted values on load.
    return std::isfinite(item.l) && std::isfinite(item.t) && std::isfinite(item.r) && std::isfinite(item.b)
        && item.l >= 0.0 && item.t >= 0.0 && item.r <= 1.0 && item.b <= 1.0 && item.l <= item.r && item.t <= item.b;
}

static QByteArray decompressBounded(const QByteArray& compressed)
{
    // The first four bytes declare the uncompressed size. Validate that size
    // before allocating, then let zlib fill only the bounded destination.
    if (compressed.size() < 4)
        return { };

    const auto* header = reinterpret_cast<const uchar*>(compressed.constData());
    const quint32 declaredSize =
        (quint32(header[0]) << 24) | (quint32(header[1]) << 16) | (quint32(header[2]) << 8) | quint32(header[3]);
    if (declaredSize == 0 || declaredSize > OcrConstant::MAX_CACHE_RAW_BYTES)
        return { };

    QByteArray raw(static_cast<qsizetype>(declaredSize), Qt::Uninitialized);
    uLong rawSize = declaredSize;
    const auto* source = reinterpret_cast<const uchar*>(compressed.constData()) + 4;
    const auto sourceSize = static_cast<uLong>(compressed.size() - 4);
    if (::uncompress(reinterpret_cast<uchar*>(raw.data()), &rawSize, source, sourceSize) != Z_OK)
        return { };
    raw.resize(static_cast<qsizetype>(rawSize));
    return raw;
}

static QString sanitizeFilenamePart(const QString& str)
{
    // Cache paths are derived from document and language identifiers. Reduce
    // them to safe filename components before composing a path.
    if (str.isEmpty()) {
        return QString();
    }

    QString safe;
    safe.reserve(str.size());

    for (const QChar& ch : str) {
        if (ch.isLetterOrNumber() || ch == QChar('-') || ch == QChar('_') || ch == QChar('.')) {
            safe.append(ch);
        } else {
            if (!safe.isEmpty() && !safe.endsWith(QChar('_'))) {
                safe.append(QChar('_'));
            }
        }
    }

    while (!safe.isEmpty() && (safe.endsWith(QChar('_')) || safe.endsWith(QChar('.')))) {
        safe.chop(1);
    }
    while (!safe.isEmpty() && (safe.startsWith(QChar('_')) || safe.startsWith(QChar('.')))) {
        safe.remove(0, 1);
    }

    return safe;
}

std::optional<CacheKey> Cache::normalizeKey(const QString& docHash, const QString& lang, int dpi)
{
    // Normalize equivalent language/DPI spellings to one stable cache key.
    const QString safeHash = sanitizeFilenamePart(docHash).toLower();
    if (safeHash.isEmpty())
        return std::nullopt;

    const QString rawLang = sanitizeFilenamePart(stripLangSuffix(lang).toLower());
    if (rawLang.isEmpty())
        return std::nullopt;

    // A DPI suffix is accepted for compatibility with Tesseract language names;
    // an explicit dpi argument takes precedence over the suffix.
    const qsizetype underscoreIdx = rawLang.lastIndexOf(QLatin1Char('_'));
    QString baseLang = rawLang;
    int parsedDpi = 0;
    if (underscoreIdx > 0 && rawLang.mid(underscoreIdx + 1).endsWith(QLatin1String("dpi"))) {
        bool ok = false;
        const QString suffix = rawLang.mid(underscoreIdx + 1);
        const int p = suffix.left(suffix.length() - 3).toInt(&ok);
        if (ok) {
            baseLang = rawLang.left(underscoreIdx);
            parsedDpi = p;
        }
    }

    const int targetDpi = (dpi > 0) ? dpi : parsedDpi;
    return CacheKey { safeHash, baseLang, targetDpi };
}

QString Cache::getCacheFilePath(const CacheKey& key, int pageNum)
{
    // Keep one file per document page and OCR configuration. The normalized key
    // has already removed path separators and other unsafe filename characters.
    if (pageNum < 0)
        return QString();

    const QString cleanLang =
        (key.dpi > 0) ? QStringLiteral("%1_%2dpi").arg(key.language, QString::number(key.dpi)) : key.language;

    const QString docCacheDir = directory(QStringLiteral("ocr_cache")) + QLatin1Char('/') + key.documentHash;
    return docCacheDir + QStringLiteral("/p") + QString::number(pageNum) + QStringLiteral("_") + cleanLang
        + QStringLiteral(".bin");
}

QString Cache::getCacheFilePath(const QString& docHash, int pageNum, const QString& lang, int dpi)
{
    const auto key = normalizeKey(docHash, lang, dpi);
    if (!key || pageNum < 0)
        return QString();
    return getCacheFilePath(*key, pageNum);
}

static std::optional<quint32> readCacheItemCount(const QByteArray& data)
{
    // Header validation happens before payload reads so malformed files are
    // rejected without trusting their item count or compressed size.
    const qint64 fileSize = data.size();
    if (!isCacheFileSizeValid(fileSize))
        return std::nullopt;

    uchar headerBuf[OcrConstant::CACHE_HEADER_BYTES];
    if (data.size() < OcrConstant::CACHE_HEADER_BYTES)
        return std::nullopt;
    std::copy_n(data.constData(), OcrConstant::CACHE_HEADER_BYTES, reinterpret_cast<char*>(headerBuf));

    const quint32 magic = (quint32(headerBuf[0]) << 24) | (quint32(headerBuf[1]) << 16) | (quint32(headerBuf[2]) << 8)
        | quint32(headerBuf[3]);
    const quint32 count = (quint32(headerBuf[4]) << 24) | (quint32(headerBuf[5]) << 16) | (quint32(headerBuf[6]) << 8)
        | quint32(headerBuf[7]);

    // The header is deliberately parsed without QDataStream so its fixed
    // big-endian layout can be checked before any compressed data is touched.
    if (magic != OcrConstant::CACHE_MAGIC || count > OcrConstant::MAX_ITEMS_PER_PAGE)
        return std::nullopt;

    if (count == 0 && fileSize != OcrConstant::CACHE_HEADER_BYTES)
        return std::nullopt;

    if (count > 0 && fileSize <= OcrConstant::CACHE_HEADER_BYTES)
        return std::nullopt;

    return count;
}

static CacheLoadResult loadFromFile(const QString& cacheFilePath)
{
    // A cache entry is usable only after header, decompression, stream, and
    // coordinate validation. Any failure removes the entry for self-healing.
    if (cacheFilePath.isEmpty()) {
        return { false, { } };
    }

    const auto data =
        readBounded(cacheFilePath, OcrConstant::CACHE_HEADER_BYTES + OcrConstant::MAX_CACHE_COMPRESSED_BYTES);
    if (!data) {
        QFile::remove(cacheFilePath);
        return { false, { } };
    }

    const auto itemCount = readCacheItemCount(*data);
    if (!itemCount) {
        QFile::remove(cacheFilePath);
        return { false, { } };
    }

    if (*itemCount == 0) {
        return { true, { } };
    }

    const QByteArray compressedData = data->sliced(OcrConstant::CACHE_HEADER_BYTES);

    // The compressed section is bounded by readBounded; decompression adds a
    // separate raw-size bound before QDataStream sees the result.
    const QByteArray rawData = decompressBounded(compressedData);
    if (rawData.isEmpty()) {
        QFile::remove(cacheFilePath);
        return { false, { } };
    }

    QDataStream in(rawData);
    in.setVersion(QDataStream::Qt_6_0);

    QVector<CacheItem> boxes;
    boxes.reserve(*itemCount);
    // Read exactly the number of records declared by the header. QDataStream
    // status and coordinate validation protect each record independently.
    for (quint32 i = 0; i < *itemCount; ++i) {
        QString chStr;
        double l = 0, t = 0, r = 0, b = 0;
        in >> chStr >> l >> t >> r >> b;

        const CacheItem item { chStr, l, t, r, b };
        if (in.status() != QDataStream::Ok || !isValidCacheItem(item)) {
            QFile::remove(cacheFilePath);
            return { false, { } };
        }

        boxes.append(item);
    }

    return { true, std::move(boxes) };
}

static QList<CacheKey> candidateKeys(const CacheKey& key)
{
    // Exact DPI is preferred. A higher-DPI result can satisfy a lower-DPI
    // request, but a lower-quality result must never satisfy a higher request.
    QList<CacheKey> list { key };
    if (key.dpi > 0) {
        for (int higher :
             { static_cast<int>(OcrConstant::DPI_ACCURACY), static_cast<int>(OcrConstant::DPI_BALANCED) }) {
            if (higher > key.dpi) {
                list.append(CacheKey { key.documentHash, key.language, higher });
            }
        }
    }
    return list;
}

static QList<QString> candidateFilePaths(const CacheKey& key, int pageNum)
{
    if (pageNum < 0)
        return { };

    const auto keys = candidateKeys(key);
    QList<QString> paths;
    paths.reserve(keys.size());
    for (const auto& candidate : keys) {
        paths.append(Cache::getCacheFilePath(candidate, pageNum));
    }
    return paths;
}

CacheLoadResult Cache::load(const CacheKey& key, int pageNum)
{
    // Prefer the requested DPI, then allow only configured higher-quality
    // candidates. A valid empty result remains present=true.
    for (const QString& path : candidateFilePaths(key, pageNum)) {
        if (QFile::exists(path)) {
            const auto result = loadFromFile(path);
            if (result.present) {
                return result;
            }
        }
    }
    return { false, { } };
}

CacheLoadResult Cache::load(const QString& docHash, int pageNum, const QString& lang, int dpi)
{
    const auto key = normalizeKey(docHash, lang, dpi);
    return key ? load(*key, pageNum) : CacheLoadResult { false, { } };
}

static std::optional<QByteArray> serializeCacheFile(const QVector<CacheItem>& items)
{
    // Serialize the payload separately so raw and compressed resource limits
    // can be checked before the file header and bytes are committed.
    QByteArray fileData;
    if (items.isEmpty()) {
        fileData.reserve(OcrConstant::CACHE_HEADER_BYTES);
        QDataStream headerStream(&fileData, QIODevice::WriteOnly);
        headerStream.setVersion(QDataStream::Qt_6_0);
        headerStream << OcrConstant::CACHE_MAGIC << quint32(0);
        return fileData;
    }

    // Step A: Serialize items (without magic and count).
    QByteArray rawBuffer;
    QDataStream out(&rawBuffer, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    for (const auto& item : items) {
        out << item.ch << item.l << item.t << item.r << item.b;
    }

    if (out.status() != QDataStream::Ok) {
        MU_LOG(warning, "Mu::Plugin::OCR", "failed to serialize OCR cache data");
        return std::nullopt;
    }

    if (rawBuffer.size() > OcrConstant::MAX_CACHE_RAW_BYTES) {
        MU_LOG(warning, "Mu::Plugin::OCR", "OCR cache raw payload exceeds limit");
        return std::nullopt;
    }

    // Step B: Compress the items payload using zlib level 1 (fastest compression).
    const QByteArray compressedData = qCompress(rawBuffer, 1);
    if (compressedData.isEmpty() || compressedData.size() > OcrConstant::MAX_CACHE_COMPRESSED_BYTES) {
        MU_LOG(warning, "Mu::Plugin::OCR", "compressed OCR cache exceeds limit");
        return std::nullopt;
    }

    // Step C: Assemble [magic][count][compressedData].
    fileData.reserve(OcrConstant::CACHE_HEADER_BYTES + compressedData.size());
    QDataStream headerStream(&fileData, QIODevice::WriteOnly);
    headerStream.setVersion(QDataStream::Qt_6_0);
    headerStream << OcrConstant::CACHE_MAGIC << quint32(items.size());
    fileData.append(compressedData);
    return fileData;
}

bool Cache::save(const CacheKey& key, int pageNum, const QVector<CacheItem>& items)
{
    // Reject invalid page/item counts before creating directories or files.
    if (items.size() > static_cast<qsizetype>(OcrConstant::MAX_ITEMS_PER_PAGE) || pageNum < 0) {
        MU_LOG(
            warning, "Mu::Plugin::OCR", "attempted to save invalid OCR items count: " + std::to_string(items.size()));
        return false;
    }

    const QString cacheFilePath = getCacheFilePath(key, pageNum);
    if (cacheFilePath.isEmpty()) {
        return false;
    }

    const auto fileData = serializeCacheFile(items);
    if (!fileData) {
        return false;
    }

    return writeAtomically(cacheFilePath, *fileData);
}

bool Cache::save(const QString& docHash, int pageNum, const QString& lang, int dpi, const QVector<CacheItem>& items)
{
    const auto key = normalizeKey(docHash, lang, dpi);
    return key ? save(*key, pageNum, items) : false;
}

QString Cache::stripLangSuffix(const QString& lang)
{
    QString clean = lang;
    if (clean.endsWith(QLatin1String(".traineddata")))
        clean.chop(12);
    return clean;
}

float Cache::qualityToDpi(int quality)
{
    // Quality values come from the generator settings; unknown values use the
    // balanced default so they do not accidentally request the slowest mode.
    switch (quality) {
    case 0: // Speed
        return OcrConstant::DPI_FAST;
    case 2: // Accuracy
        return OcrConstant::DPI_ACCURACY;
    default:
        return OcrConstant::DPI_BALANCED;
    }
}

QVector<CacheItem> Cache::convertToCacheItems(const std::vector<Model::TextBox>& boxes)
{
    // Preserve the worker's normalized geometry and UTF-8 text without routing
    // the cache conversion through Qt model objects.
    QVector<CacheItem> items;
    items.reserve(static_cast<qsizetype>(boxes.size()));
    for (const auto& box : boxes) {
        items.append({ QString::fromStdString(box.text), box.left, box.top, box.right, box.bottom });
    }
    return items;
}

} // namespace Mu::Plugin::Caching::OCR
