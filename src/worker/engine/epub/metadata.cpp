// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/document_fingerprint.hpp"
#include "engine/epub/document.hpp"
#include "engine/mupdf_helpers.hpp"

#include <algorithm>
#include <array>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
}

#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Document Metadata Extraction
// =============================================================================

DocumentMetadata EpubDocument::metadata(const std::vector<std::string>& keys, std::string* error) const
{
    DocumentMetadata result;
    if (!m_document) {
        fail(error, "document is unavailable");
        return result;
    }
    result.pageCount = m_pageCount;
    result.mimeType = "application/epub+zip";

    constexpr std::array<std::pair<const char*, const char*>, 5> knownKeys { {
        { "title", FZ_META_INFO_TITLE },
        { "author", FZ_META_INFO_AUTHOR },
        { "format", FZ_META_FORMAT },
        { "publisher", "info:Publisher" },
        { "language", "info:Language" },
    } };

    const auto wanted = [&keys](const char* name) {
        return keys.empty() || std::find(keys.begin(), keys.end(), name) != keys.end();
    };

    // EPUB documents carry no xref structure and are never repaired. The
    // repair flag is excluded from the metadata hash, which covers document
    // identity only.
    if (wanted("repaired"))
        result.values.emplace("repaired", "false");

    const bool wantHash = wanted("hash");
    DocumentFingerprint hash;
    if (wantHash)
        hash.addMetadataField("pageCount", std::to_string(m_pageCount));

    for (const auto& [name, mupdfKey] : knownKeys) {
        if (!wantHash && !wanted(name))
            continue;
        std::string value;
        bool found = false;
        if (!lookupMetadataValue(m_context, m_document, mupdfKey, value, found, error))
            return { };
        if (!found)
            continue;
        if (wantHash)
            hash.addMetadataField(name, value);
        if (wanted(name))
            result.values.emplace(name, std::move(value));
    }

    if (wantHash)
        result.values.emplace("hash", hash.hexDigest());
    return result;
}

} // namespace Mu::Worker::Engine
