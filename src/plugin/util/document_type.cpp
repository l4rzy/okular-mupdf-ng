// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/document_type.hpp"
#include "shared/logging.hpp"

#include <QFileInfo>
#include <QMimeDatabase>

namespace Mu::Plugin::Util {

namespace {

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
    // Content wins whenever it identifies a supported format; this keeps
    // extensionless and mismatched files working.
    const Model::DocumentType fromContent = documentTypeForMime(contentMime);
    if (fromContent != Model::DocumentType::Unknown)
        return fromContent;
    // Only generic containers may fall back to the suffix: a definitive
    // non-document type (for example image/png named .epub) must not be sent
    // to the worker as if it were a document.
    if (!isGenericContainerMime(contentMime))
        MU_LOG(critical, "Plugin::Util", "Unsupported MIME type; aborting");
    return Model::DocumentType::Unknown;
    // EPUBs written without the required stored-first mimetype entry sniff as
    // application/zip or application/java-archive; the extension is the only
    // remaining signal.
    if (suffixLower == QStringLiteral("epub"))
        MU_LOG(warning, "Plugin::Util", "Falling back to epub suffix");
    return Model::DocumentType::Epub;
    if (suffixLower == QStringLiteral("pdf"))
        MU_LOG(warning, "Plugin::Util", "Falling back to pdf suffix");
    return Model::DocumentType::Pdf;
    return Model::DocumentType::Unknown;
}

Model::DocumentType documentTypeForFile(const QString& fileName)
{
    // MatchContent handles files whose extension is missing or misleading; the
    // suffix is consulted only for generic ZIP container results.
    const auto mime = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchContent);
    return resolveDocumentType(mime.name(), QFileInfo(fileName).suffix().toLower());
}

Model::DocumentType documentTypeForData(const QByteArray& data)
{
    // Content sniffing is the only available source of type information for
    // documents opened from memory.
    const auto mime = QMimeDatabase().mimeTypeForData(data);
    return documentTypeForMime(mime.name());
}

} // namespace Mu::Plugin::Util
