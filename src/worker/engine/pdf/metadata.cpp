// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

namespace {

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
        // MuPDF reports the required buffer size first. Keep the allocation inside
        // its exception boundary, then copy the result into an owning std::string
        // only after all fallible C API calls have completed.
        required = fz_lookup_metadata(context, document, key, nullptr, 0);
        if (required > 1 && required <= 1024 * 1024) {
            buffer = static_cast<char*>(fz_malloc(context, static_cast<std::size_t>(required)));
            if (fz_lookup_metadata(context, document, key, buffer, static_cast<std::size_t>(required)) >= required)
                found = true;
        }
    }
    fz_always(context)
    {
        // The fz allocator owns this temporary buffer; free it on every path that
        // does not transfer its contents to the C++ string below.
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

    // `required` includes MuPDF's terminating NUL. The C++ value stores only the
    // metadata bytes, while the buffer remains valid until it is explicitly freed.
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

// Metadata is read through MuPDF's document API rather than by reaching
// into pdf_document internals. That keeps this boundary usable for every
// document type supported by MuPDF and bounds untrusted metadata values.
DocumentMetadata PdfDocument::metadata(const std::vector<std::string>& keys, std::string* error) const
{
    DocumentMetadata result;
    if (!m_document || m_locked) {
        fail(error, "document is unavailable");
        return result;
    }

    result.pageCount = m_pageCount;
    result.mimeType = "application/pdf";

    constexpr std::array<std::pair<const char*, const char*>, 8> knownKeys { {
        { "title", FZ_META_INFO_TITLE },
        { "subject", FZ_META_INFO_SUBJECT },
        { "author", FZ_META_INFO_AUTHOR },
        { "keywords", FZ_META_INFO_KEYWORDS },
        { "creator", FZ_META_INFO_CREATOR },
        { "producer", FZ_META_INFO_PRODUCER },
        { "format", FZ_META_FORMAT },
        { "security", FZ_META_ENCRYPTION },
    } };

    const auto wanted = [&keys](const char* name) {
        return keys.empty() || std::find(keys.begin(), keys.end(), name) != keys.end();
    };

    const bool wantHash = wanted("hash");
    fz_sha256 hash { };
    constexpr unsigned char separator = 0;

    // Hash the same canonical sequence regardless of which metadata keys the
    // caller requests; filtering changes the response, not the document hash.
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

    if (wanted("signatureCount")) {
        std::size_t signatureCount = 0;
        for (int page = 0; page < m_pageCount; ++page) {
            fz_page* nativePage = loadPage(page, error);
            if (!nativePage)
                return { };

            std::size_t pageSignatureCount = 0;
            fz_var(pageSignatureCount);
            fz_try(m_context)
            {
                const fz_rect bounds = fz_bound_page(m_context, nativePage);
                const auto signatures = extractPageSignatures(nativePage, bounds);
                pageSignatureCount = static_cast<std::size_t>(
                    std::count_if(signatures.cbegin(), signatures.cend(), [](const SignatureField& field) {
                        return field.signedField;
                    }));
            }
            fz_always(m_context)
            {
                fz_drop_page(m_context, nativePage);
            }
            fz_catch(m_context)
            {
                fail(error, fz_caught_message(m_context));
                return { };
            }
            signatureCount += pageSignatureCount;
        }
        result.values.emplace("signatureCount", std::to_string(signatureCount));
    }

    // Finalize hexadecimal metadata digest after all fallible MuPDF lookups
    // have completed, so C++ string/map lifetimes are ordinary stack lifetimes.
    if (wantHash) {
        std::array<unsigned char, 32> digest { };
        fz_sha256_final(&hash, digest.data());
        constexpr char hex[] = "0123456789abcdef";
        std::string value;
        value.reserve(digest.size() * 2);
        for (const unsigned char byte : digest) {
            value.push_back(hex[byte >> 4]);
            value.push_back(hex[byte & 0x0f]);
        }
        result.values.emplace("hash", std::move(value));
    }

    return result;
}

} // namespace Mu::Worker::Engine
