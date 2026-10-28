// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/document_type.hpp"
#include "shared/logging.hpp"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QtEndian>

namespace Mu::Plugin::Util {

namespace {

Model::DocumentType detectMobiType(const QByteArray& database, const QByteArray& header)
{
    if (database.size() < 94 || database.mid(60, 8) != "BOOKMOBI" || header.size() < 40 || header.mid(16, 4) != "MOBI")
        return Model::DocumentType::Mobi;
    const auto version = qFromBigEndian<quint32>(header.constData() + 36);
    return version == 8 ? Model::DocumentType::Kindle : Model::DocumentType::Mobi;
}

std::uint32_t readFirstRecordOffset(const QByteArray& database)
{
    if (database.size() < 82)
        return 0;
    return qFromBigEndian<quint32>(database.constData() + 78);
}

// ZIP-based types that carry no document information on their own. EPUB
// archives are ZIP containers, so these defer to the file suffix; a real
// .zip/.jar remains Unknown.
bool isZipContainerMime(const QString& mime) noexcept
{
    return mime == QStringLiteral("application/zip") || mime == QStringLiteral("application/x-zip")
        || mime == QStringLiteral("application/x-zip-compressed") || mime == QStringLiteral("application/java-archive");
}

Model::DocumentType detectDocumentType(QIODevice& device, const QString& suffixLower = { })
{
    const auto database = device.peek(94);
    if (database.startsWith("CONT"))
        return Model::DocumentType::Kindle;

    const auto mime = QMimeDatabase().mimeTypeForData(&device);
    const auto type = resolveDocumentType(mime.name(), suffixLower);
    if (type != Model::DocumentType::Mobi || !device.seek(readFirstRecordOffset(database)))
        return type;
    return detectMobiType(database, device.read(40));
}

} // namespace

Model::DocumentType documentTypeForFile(const QString& fileName)
{
    QFile file(fileName);
    const auto suffix = QFileInfo(fileName).suffix().toLower();
    if (file.open(QIODevice::ReadOnly))
        return detectDocumentType(file, suffix);

    // Preserve Qt's MIME fallback when the source cannot be opened.
    const auto mime = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchContent);
    return resolveDocumentType(mime.name(), suffix);
}

Model::DocumentType documentTypeForData(const QByteArray& data)
{
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly))
        return Model::DocumentType::Unknown;
    return detectDocumentType(buffer);
}

Model::DocumentType resolveDocumentType(const QString& contentMime, const QString& suffixLower) noexcept
{
    // Content wins whenever it identifies a supported format.
    const Model::DocumentType fromContent = documentTypeForMime(contentMime);
    if (fromContent != Model::DocumentType::Unknown)
        return fromContent;

    // Recognized non-document MIME types cannot use suffix fallbacks.
    // Unidentified binary content permits EPUB, MOBI and Kindle suffixes;
    // ZIP containers permit only the EPUB fallback.
    const bool isUnidentifiedBinary = contentMime == QStringLiteral("application/octet-stream");
    if (!isUnidentifiedBinary && !isZipContainerMime(contentMime)) {
        MU_LOG(critical, "Mu::Plugin", "Unsupported common container type; aborting");
        return Model::DocumentType::Unknown;
    }

    // EPUBs written without the required stored-first mimetype entry sniff as
    // application/zip or application/java-archive; the extension is the only
    // remaining signal.
    if (suffixLower == QStringLiteral("epub")) {
        MU_LOG(warning, "Mu::Plugin", "Falling back to epub suffix");
        return Model::DocumentType::Epub;
    }

    if (isUnidentifiedBinary) {
        if (suffixLower == QStringLiteral("mobi"))
            return Model::DocumentType::Mobi;
        if (suffixLower == QStringLiteral("azw3") || suffixLower == QStringLiteral("kfx"))
            return Model::DocumentType::Kindle;
    }

    MU_LOG(critical, "Mu::Plugin", "Unsupported MIME type; aborting");
    return Model::DocumentType::Unknown;
}

} // namespace Mu::Plugin::Util
