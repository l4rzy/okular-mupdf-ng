// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/epub/document.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/fitz/version.h>
}

#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

namespace {

// Reads one metadata value through a C-owned buffer: allocation and lookup
// stay inside the exception boundary, the std::string copy happens after.
bool lookupMetadataValue(
    fz_context* context, fz_document* document, const char* key, std::string& value, bool& found, std::string* error)
{
    char* buffer = nullptr;
    int required = 0;
    found = false;
    fz_var(buffer);
    fz_var(required);
    fz_var(found);

    fz_try(context)
    {
        required = fz_lookup_metadata(context, document, key, nullptr, 0);
        if (required > 1 && required <= 1024 * 1024) {
            buffer = static_cast<char*>(fz_malloc(context, static_cast<std::size_t>(required)));
            if (fz_lookup_metadata(context, document, key, buffer, static_cast<std::size_t>(required)) >= required)
                found = true;
        }
    }
    fz_always(context)
    {
        if (!found && buffer)
            fz_free(context, buffer);
    }
    fz_catch(context)
    {
        if (error)
            *error = fz_caught_message(context);
        return false;
    }

    if (!found)
        return true;

    try {
        value.assign(buffer, static_cast<std::size_t>(required - 1));
    } catch (...) {
        fz_free(context, buffer);
        throw;
    }
    fz_free(context, buffer);
    return true;
}

} // namespace

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

    // Engine-common values: the MuPDF version compiled into this worker binary
    // is authoritative for the generator, and the runtime records whether the
    // open required a non-empty password (always false for EPUB). They are
    // excluded from the metadata hash, which covers document identity only.
    if (wanted("engineVersion"))
        result.values.emplace("engineVersion", FZ_VERSION);
    // EPUB documents carry no xref structure and are never repaired.
    if (wanted("repaired"))
        result.values.emplace("repaired", "false");

    const bool wantHash = wanted("hash");
    fz_sha256 hash { };
    constexpr unsigned char separator = 0;

    // Initialize SHA-256 metadata hash accumulator
    if (wantHash) {
        fz_sha256_init(&hash);
        const std::string pageCount = std::to_string(m_pageCount);
        constexpr std::string_view pageCountKey = "pageCount:";
        fz_sha256_update(&hash, reinterpret_cast<const unsigned char*>(pageCountKey.data()), pageCountKey.size());
        fz_sha256_update(&hash, reinterpret_cast<const unsigned char*>(pageCount.data()), pageCount.size());
        fz_sha256_update(&hash, &separator, 1);
    }

    for (const auto& [name, mupdfKey] : knownKeys) {
        std::string value;
        bool found = false;
        if (!lookupMetadataValue(m_context, m_document, mupdfKey, value, found, error))
            return { };
        if (!found)
            continue;
        if (wantHash) {
            const std::string field = std::string(name) + ":";
            fz_sha256_update(&hash, reinterpret_cast<const unsigned char*>(field.data()), field.size());
            fz_sha256_update(&hash, reinterpret_cast<const unsigned char*>(value.data()), value.size());
            fz_sha256_update(&hash, &separator, 1);
        }
        if (wanted(name))
            result.values.emplace(name, std::move(value));
    }

    // Finalize hexadecimal digest
    if (wantHash) {
        std::array<unsigned char, 32> digest { };
        fz_sha256_final(&hash, digest.data());
        constexpr char hex[] = "0123456789abcdef";
        std::string digestValue;
        digestValue.reserve(digest.size() * 2);
        for (const unsigned char byte : digest) {
            digestValue.push_back(hex[byte >> 4]);
            digestValue.push_back(hex[byte & 0x0f]);
        }
        result.values.emplace("hash", std::move(digestValue));
    }
    return result;
}

} // namespace Mu::Worker::Engine
