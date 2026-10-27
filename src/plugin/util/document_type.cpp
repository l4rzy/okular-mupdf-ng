// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/document_type.hpp"
#include "shared/logging.hpp"

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
bool isGenericContainerMime(const QString& mime) noexcept
{
    return mime == QStringLiteral("application/zip") || mime == QStringLiteral("application/x-zip")
        || mime == QStringLiteral("application/x-zip-compressed") || mime == QStringLiteral("application/java-archive")
        || mime == QStringLiteral("application/octet-stream");
}

} // namespace

Model::DocumentType resolveDocumentType(const QString& contentMime, const QString& suffixLower) noexcept
{
    // Content wins whenever it identifies a supported format.
    const Model::DocumentType fromContent = documentTypeForMime(contentMime);
    if (fromContent != Model::DocumentType::Unknown)
        return fromContent;

    // Only generic containers may fall back to the suffix: a definitive
    // non-document type (for example image/png named .epub) must not be sent
    // to the worker as if it were a document.
    if (!isGenericContainerMime(contentMime)) {
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

    if (contentMime == QStringLiteral("application/octet-stream") && suffixLower == QStringLiteral("mobi"))
        return Model::DocumentType::Mobi;
    if (contentMime == QStringLiteral("application/octet-stream")
        && (suffixLower == QStringLiteral("azw3") || suffixLower == QStringLiteral("kfx")))
        return Model::DocumentType::Kindle;

    if (suffixLower == QStringLiteral("pdf")) {
        MU_LOG(warning, "Mu::Plugin", "Falling back to pdf suffix");
        return Model::DocumentType::Pdf;
    }

    MU_LOG(critical, "Mu::Plugin", "Unsupported MIME type; aborting");
    return Model::DocumentType::Unknown;
}

Model::DocumentType documentTypeForFile(const QString& fileName)
{
    // MatchContent handles files whose extension is missing or misleading.
    QFile file(fileName);
    const auto database = file.open(QIODevice::ReadOnly) ? file.read(94) : QByteArray();
    if (database.startsWith("CONT"))
        return Model::DocumentType::Kindle;
    const auto mime = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchContent);
    const auto type = resolveDocumentType(mime.name(), QFileInfo(fileName).suffix().toLower());
    if (type != Model::DocumentType::Mobi)
        return type;
    if (!file.isOpen())
        return type;
    if (!file.seek(readFirstRecordOffset(database)))
        return type;
    return detectMobiType(database, file.read(40));
}

Model::DocumentType documentTypeForData(const QByteArray& data)
{
    // Content sniffing is the only available source of type information for
    // documents opened from memory.
    if (data.startsWith("CONT"))
        return Model::DocumentType::Kindle;
    const auto mime = QMimeDatabase().mimeTypeForData(data);
    const auto type = documentTypeForMime(mime.name());
    if (type != Model::DocumentType::Mobi)
        return type;
    const auto database = data.left(94);
    return detectMobiType(database, data.mid(readFirstRecordOffset(database), 40));
}

} // namespace Mu::Plugin::Util
