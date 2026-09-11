// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_DOCUMENT_TYPE_HPP
#define MU_PLUGIN_UTIL_DOCUMENT_TYPE_HPP

#include <QByteArray>
#include <QString>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Util {

/// Converts a detected MIME type to a document type.
inline Model::DocumentType documentTypeForMime(const QString& mime)
{
    return Model::documentTypeFromMime(mime.toStdString());
}

/**
 * Resolves a document type from a sniffed MIME type and a lowercased file
 * suffix. Definitive content wins; generic ZIP container types (a common
 * result for non-compliant EPUB archives) fall back to the suffix.
 */
Model::DocumentType resolveDocumentType(const QString& contentMime, const QString& suffixLower) noexcept;

/// Detects the document type of a file by content, with a suffix fallback.
Model::DocumentType documentTypeForFile(const QString& fileName);

/// Detects the document type of in-memory data; no suffix is available.
Model::DocumentType documentTypeForData(const QByteArray& data);

} // namespace Mu::Plugin::Util

#endif // MU_PLUGIN_UTIL_DOCUMENT_TYPE_HPP
