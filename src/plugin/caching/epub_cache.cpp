// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/epub_cache.hpp"

#include "plugin/caching/cache_file.hpp"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include "shared/protocol/limits.hpp"

namespace Mu::Plugin::Caching::EPUB {

namespace {

// Cache files are versioned so the reader can reject stale or incompatible
// layouts without trying to interpret their payload as current data.
constexpr quint32 V1Magic = 0x45504131; // 'EPA1'
constexpr quint32 V2Magic = 0x45504132; // 'EPA2'
constexpr quint32 V1Version = 1;
constexpr quint32 V2Version = 2;
constexpr qint64 V1HeaderBytes = 28;
constexpr qint64 V2HeaderBytes = 32;
constexpr qint64 MaxAcceleratorBytes = static_cast<qint64>(Model::MaxEpubAcceleratorBytes);
constexpr qint64 MaxOutlineCompressedBytes = 8LL * 1024LL * 1024LL;
constexpr qint64 MaxOutlineRawBytes = 64LL * 1024LL * 1024LL;
constexpr qint64 MaxCacheBytes = V2HeaderBytes + 2 * 8 + MaxAcceleratorBytes + MaxOutlineCompressedBytes;
constexpr quint32 AcceleratorSection = 1;
constexpr quint32 OutlineSection = 2;
constexpr std::size_t MaxOutlineDepth = 64;
constexpr std::size_t MaxOutlineNodes = 50'000;

QString sourcePath(const QString& path)
{
    // Canonicalize existing paths so aliases and relative spellings share one
    // cache entry; absolutePath remains a useful fallback for unusual sources.
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QByteArray layoutFingerprint(const Model::DocumentSettings& settings)
{
    // Only settings that affect EPUB pagination or styling belong in this key.
    // Hash the CSS rather than embedding its potentially large contents.
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << settings.epub.fontSize << static_cast<quint8>(settings.epub.pageSize)
           << static_cast<quint8>(settings.epub.fontFamily)
           << QCryptographicHash::hash(QByteArray::fromStdString(settings.epub.customCssBase64),
                                       QCryptographicHash::Sha256);
    return encoded;
}

bool writeBytes(QDataStream& stream, const QByteArray& bytes)
{
    // Length-prefix byte strings with a fixed-width value so readers can check
    // the declared size before allocating or consuming the payload.
    if (bytes.size() > std::numeric_limits<quint32>::max())
        return false;
    stream << static_cast<quint32>(bytes.size());
    return stream.writeRawData(bytes.constData(), bytes.size()) == bytes.size();
}

bool readBytes(QDataStream& stream, qint64 maxBytes, QByteArray* result)
{
    if (!result)
        return false;
    quint32 size = 0;
    stream >> size;
    // Validate the length against both the section limit and remaining input
    // before allocating the destination buffer.
    if (stream.status() != QDataStream::Ok || size > maxBytes || !stream.device()
        || stream.device()->bytesAvailable() < static_cast<qint64>(size))
        return false;

    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (stream.readRawData(bytes.data(), static_cast<int>(size)) != static_cast<int>(size))
        return false;
    *result = std::move(bytes);
    return true;
}

bool writeOutlineNodes(QDataStream& stream,
                       const std::vector<Model::OutlineNode>& nodes,
                       std::size_t depth,
                       std::size_t* total)
{
    // Apply depth and aggregate-node limits during serialization as well as
    // deserialization so a caller cannot create an oversized cache file.
    if (!total || (!nodes.empty() && depth > MaxOutlineDepth) || nodes.size() > std::numeric_limits<quint32>::max()
        || nodes.size() > MaxOutlineNodes - *total)
        return false;
    *total += nodes.size();
    stream << static_cast<quint32>(nodes.size());
    for (const auto& node : nodes) {
        if (!writeBytes(stream, QByteArray::fromStdString(node.title)))
            return false;
        stream << static_cast<quint8>(node.open ? 1 : 0) << static_cast<quint8>(node.link.external ? 1 : 0);
        if (!writeBytes(stream, QByteArray::fromStdString(node.link.uri)))
            return false;
        stream << node.link.viewport.page << node.link.viewport.normalizedX << node.link.viewport.normalizedY
               << node.link.viewport.coordinateMask << static_cast<quint8>(node.link.valid ? 1 : 0);
        if (!writeOutlineNodes(stream, node.children, depth + 1, total))
            return false;
    }
    return stream.status() == QDataStream::Ok;
}

std::optional<QByteArray> serializeOutline(const std::vector<Model::OutlineNode>& outline)
{
    // Serialize to an intermediate bounded representation before compression;
    // this limits both the uncompressed work and the stored compressed section.
    QByteArray raw;
    QDataStream stream(&raw, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    std::size_t total = 0;
    if (!writeOutlineNodes(stream, outline, 0, &total) || raw.size() > MaxOutlineRawBytes)
        return std::nullopt;

    const QByteArray compressed = qCompress(raw, 1);
    if (compressed.isEmpty() || compressed.size() > MaxOutlineCompressedBytes)
        return std::nullopt;
    return compressed;
}

bool readOutlineNodes(QDataStream& stream,
                      std::vector<Model::OutlineNode>* nodes,
                      std::size_t depth,
                      std::size_t* total)
{
    if (!nodes || !total)
        return false;
    quint32 count = 0;
    stream >> count;
    // Validate each child count against the remaining global node budget before
    // reserving vector capacity or descending recursively.
    if (stream.status() != QDataStream::Ok || (count > 0 && depth > MaxOutlineDepth) || count > MaxOutlineNodes
        || count > MaxOutlineNodes - *total)
        return false;
    *total += count;
    nodes->reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        Model::OutlineNode node;
        QByteArray bytes;
        if (!readBytes(stream, Mu::Limit::MaxString, &bytes))
            return false;
        node.title = std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        quint8 open = 0, external = 0, valid = 0;
        stream >> open >> external;
        if (open > 1 || external > 1 || stream.status() != QDataStream::Ok)
            return false;
        if (!readBytes(stream, Mu::Limit::MaxString, &bytes))
            return false;
        node.link.uri = std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        stream >> node.link.viewport.page >> node.link.viewport.normalizedX >> node.link.viewport.normalizedY
            >> node.link.viewport.coordinateMask >> valid;
        // Cache data is untrusted input: reject malformed booleans, invalid
        // page indices, and non-finite coordinates before publishing a node.
        if (valid > 1 || node.link.viewport.page < -1 || !std::isfinite(node.link.viewport.normalizedX)
            || !std::isfinite(node.link.viewport.normalizedY) || stream.status() != QDataStream::Ok)
            return false;
        node.open = open != 0;
        node.link.external = external != 0;
        node.link.valid = valid != 0;
        if (!readOutlineNodes(stream, &node.children, depth + 1, total))
            return false;
        nodes->push_back(std::move(node));
    }
    return true;
}

std::optional<std::vector<Model::OutlineNode>> deserializeOutline(const QByteArray& compressed)
{
    // qCompress stores the uncompressed size in a four-byte big-endian prefix;
    // check it before decompressing to bound memory and CPU use.
    if (compressed.size() < 4 || compressed.size() > MaxOutlineCompressedBytes)
        return std::nullopt;
    const auto* header = reinterpret_cast<const uchar*>(compressed.constData());
    const quint32 rawSize =
        (quint32(header[0]) << 24) | (quint32(header[1]) << 16) | (quint32(header[2]) << 8) | quint32(header[3]);
    if (rawSize == 0 || rawSize > MaxOutlineRawBytes)
        return std::nullopt;

    const QByteArray raw = qUncompress(compressed);
    if (raw.size() != static_cast<qsizetype>(rawSize))
        return std::nullopt;
    QDataStream stream(raw);
    stream.setVersion(QDataStream::Qt_6_0);
    std::vector<Model::OutlineNode> result;
    std::size_t total = 0;
    // Require the parser to consume exactly one outline tree, with no trailing
    // bytes that could hide an appended or version-confused payload.
    if (!readOutlineNodes(stream, &result, 0, &total) || stream.status() != QDataStream::Ok || !stream.device()
        || stream.device()->bytesAvailable() != 0)
        return std::nullopt;
    return result;
}

bool readV1(const QByteArray& data, qint64 expectedSize, qint64 expectedModified, CacheEntry* entry)
{
    if (!entry || data.size() < V1HeaderBytes)
        return false;
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0, payloadSize = 0;
    qint64 sourceSize = 0, sourceModified = 0;
    stream >> magic >> version >> sourceSize >> sourceModified >> payloadSize;
    // V1 contains only an accelerator. Its source metadata and exact payload
    // size must match before the legacy entry is accepted.
    if (stream.status() != QDataStream::Ok || magic != V1Magic || version != V1Version || sourceSize != expectedSize
        || sourceModified != expectedModified || payloadSize == 0 || payloadSize > MaxAcceleratorBytes
        || data.size() != V1HeaderBytes + static_cast<qint64>(payloadSize))
        return false;
    entry->accelerator = data.sliced(V1HeaderBytes);
    entry->outline.reset();
    return true;
}

bool readV2(const QByteArray& data, qint64 expectedSize, qint64 expectedModified, CacheEntry* entry)
{
    if (!entry || data.size() < V2HeaderBytes)
        return false;
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0, sectionCount = 0, payloadBytes = 0;
    qint64 sourceSize = 0, sourceModified = 0;
    stream >> magic >> version >> sourceSize >> sourceModified >> sectionCount >> payloadBytes;
    // The header bounds the entire section area before individual sections are
    // read, preventing a corrupt count or size from extending past the file.
    if (stream.status() != QDataStream::Ok || magic != V2Magic || version != V2Version || sourceSize != expectedSize
        || sourceModified != expectedModified || sectionCount == 0 || sectionCount > 2
        || payloadBytes > MaxCacheBytes - V2HeaderBytes || data.size() != V2HeaderBytes + payloadBytes)
        return false;

    bool sawAccelerator = false;
    bool sawOutline = false;
    for (quint32 i = 0; i < sectionCount; ++i) {
        quint32 type = 0, size = 0;
        stream >> type >> size;
        if (stream.status() != QDataStream::Ok || !stream.device() || size > stream.device()->bytesAvailable())
            return false;
        QByteArray section(static_cast<qsizetype>(size), Qt::Uninitialized);
        if (stream.readRawData(section.data(), static_cast<int>(size)) != static_cast<int>(size))
            return false;
        if (type == AcceleratorSection) {
            // Each known section may occur at most once and must remain within
            // its own content limit.
            if (sawAccelerator || size == 0 || size > MaxAcceleratorBytes)
                return false;
            entry->accelerator = std::move(section);
            sawAccelerator = true;
        } else if (type == OutlineSection) {
            if (sawOutline)
                return false;
            auto outline = deserializeOutline(section);
            if (!outline)
                return false;
            entry->outline = std::move(*outline);
            sawOutline = true;
        } else {
            // Reject unknown sections instead of silently ignoring data that
            // may belong to a different cache format.
            return false;
        }
    }
    return (sawAccelerator || sawOutline) && stream.device() && stream.device()->bytesAvailable() == 0;
}

std::optional<QByteArray> serializeSections(const CacheEntry& entry, quint32* sectionCount)
{
    // A V2 cache must contain at least one known section; optional fields let
    // accelerator and outline generation happen independently.
    if (!sectionCount || (!entry.accelerator && !entry.outline))
        return std::nullopt;
    if (entry.accelerator && (entry.accelerator->isEmpty() || entry.accelerator->size() > MaxAcceleratorBytes))
        return std::nullopt;

    QByteArray outline;
    if (entry.outline) {
        const auto serialized = serializeOutline(*entry.outline);
        if (!serialized)
            return std::nullopt;
        outline = *serialized;
    }
    *sectionCount =
        static_cast<quint32>(entry.accelerator.has_value()) + static_cast<quint32>(entry.outline.has_value());
    const qint64 payloadBytes =
        (entry.accelerator ? 8 + entry.accelerator->size() : 0) + (entry.outline ? 8 + outline.size() : 0);
    if (payloadBytes > MaxCacheBytes - V2HeaderBytes)
        return std::nullopt;

    QByteArray sections;
    sections.reserve(static_cast<qsizetype>(payloadBytes));
    QDataStream stream(&sections, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    // Section headers make each independently cached artifact length-delimited
    // and allow the reader to reject duplicate or unknown types.
    if (entry.accelerator) {
        stream << AcceleratorSection << static_cast<quint32>(entry.accelerator->size());
        stream.writeRawData(entry.accelerator->constData(), entry.accelerator->size());
    }
    if (entry.outline) {
        stream << OutlineSection << static_cast<quint32>(outline.size());
        stream.writeRawData(outline.constData(), outline.size());
    }
    return stream.status() == QDataStream::Ok ? std::optional<QByteArray>(sections) : std::nullopt;
}

bool saveEntry(const QString& cachePath, qint64 sourceSize, qint64 sourceModified, const CacheEntry& entry)
{
    quint32 sectionCount = 0;
    const auto sections = serializeSections(entry, &sectionCount);
    if (!sections)
        return false;
    QByteArray data;
    data.reserve(static_cast<qsizetype>(V2HeaderBytes + sections->size()));
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << V2Magic << V2Version << sourceSize << sourceModified << sectionCount
           << static_cast<quint32>(sections->size());
    data.append(*sections);
    // QSaveFile commits the complete file atomically, so readers never observe
    // a partially rewritten cache entry.
    return stream.status() == QDataStream::Ok && writeAtomically(cachePath, data);
}

struct SourceMetadata {
    QFileInfo source;
    QString cachePath;
};

std::optional<SourceMetadata> sourceMetadata(const QString& path, const QString& cachePath)
{
    // A missing source or derived path makes the cache unusable and avoids
    // creating entries that cannot be invalidated reliably later.
    QFileInfo source(path);
    if (!source.exists() || cachePath.isEmpty())
        return std::nullopt;
    return SourceMetadata { std::move(source), cachePath };
}

std::optional<CacheEntry> loadEntry(const SourceMetadata& metadata)
{
    // Bound the read before parsing because cache files are user-writable data,
    // not trusted application state.
    const auto data = readBounded(metadata.cachePath, MaxCacheBytes);
    if (!data)
        return std::nullopt;

    CacheEntry entry;
    const bool valid = readV1(*data, metadata.source.size(), metadata.source.lastModified().toMSecsSinceEpoch(), &entry)
        || readV2(*data, metadata.source.size(), metadata.source.lastModified().toMSecsSinceEpoch(), &entry);
    if (!valid) {
        // A malformed or stale entry is disposable; the next save can rebuild
        // it from the current document.
        QFile::remove(metadata.cachePath);
        return std::nullopt;
    }
    return entry;
}

template <typename Update> bool updateEntry(const QString& path, const QString& cachePath, Update&& update)
{
    const auto metadata = sourceMetadata(path, cachePath);
    if (!metadata)
        return false;

    // Preserve the other cache section when updating one artifact. A missing or
    // invalid old entry simply starts a fresh V2 entry.
    CacheEntry entry = loadEntry(*metadata).value_or(CacheEntry { });
    update(entry);
    return saveEntry(
        metadata->cachePath, metadata->source.size(), metadata->source.lastModified().toMSecsSinceEpoch(), entry);
}

} // namespace

QString Cache::cacheFilePath(const QString& path, const Model::DocumentSettings& settings)
{
    const QString source = sourcePath(path);
    if (source.isEmpty())
        return { };

    // SHA-256 gives a compact, collision-resistant filename without exposing
    // source paths or CSS contents in the cache directory.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(source.toUtf8());
    hash.addData(layoutFingerprint(settings));
    return directory(QStringLiteral("epub_accelerators")) + QLatin1Char('/')
        + QString::fromLatin1(hash.result().toHex()) + QStringLiteral(".bin");
}

std::optional<CacheEntry> Cache::load(const QString& path, const Model::DocumentSettings& settings)
{
    // Source size and modification time are stored in the file header and
    // checked by loadEntry, while layout settings are part of the filename.
    const auto metadata = sourceMetadata(path, cacheFilePath(path, settings));
    if (!metadata)
        return std::nullopt;
    return loadEntry(*metadata);
}

bool Cache::saveAccelerator(const QString& path, const Model::DocumentSettings& settings, const QByteArray& bytes)
{
    // Empty and oversized accelerators are never useful cache entries.
    if (bytes.isEmpty() || bytes.size() > MaxAcceleratorBytes)
        return false;
    return updateEntry(path, cacheFilePath(path, settings), [&bytes](CacheEntry& entry) { entry.accelerator = bytes; });
}

bool Cache::saveOutline(const QString& path,
                        const Model::DocumentSettings& settings,
                        const std::vector<Model::OutlineNode>& outline)
{
    // Outline serialization performs its own size, depth, and node-count checks.
    return updateEntry(path, cacheFilePath(path, settings), [&outline](CacheEntry& entry) { entry.outline = outline; });
}

} // namespace Mu::Plugin::Caching::EPUB
