// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/fitz/version.h>
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

// Converts a raw Info-dictionary date string into an ISO 8601 UTC instant
// ("2024-01-01T11:00:00Z") that QDateTime::fromString(..., Qt::ISODate)
// consumes directly. Returns an empty string when MuPDF cannot parse the
// value. Note: MuPDF interprets dates without an offset as UTC.
std::string toIsoTimestamp(fz_context* context, const std::string& raw)
{
    const auto epoch = static_cast<std::time_t>(pdf_parse_date(context, raw.c_str()));
    if (epoch < 0)
        return { };

    std::tm fields { };
    if (!gmtime_r(&epoch, &fields))
        return { };

    char buffer[21]; // "YYYY-MM-DDTHH:MM:SSZ" + NUL
    const int written = std::snprintf(buffer,
                                      sizeof buffer,
                                      "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                      fields.tm_year + 1900,
                                      fields.tm_mon + 1,
                                      fields.tm_mday,
                                      fields.tm_hour,
                                      fields.tm_min,
                                      fields.tm_sec);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof buffer)
        return { };
    return buffer;
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

    constexpr std::array<std::pair<const char*, const char*>, 10> knownKeys { {
        { "title", FZ_META_INFO_TITLE },
        { "subject", FZ_META_INFO_SUBJECT },
        { "author", FZ_META_INFO_AUTHOR },
        { "keywords", FZ_META_INFO_KEYWORDS },
        { "creator", FZ_META_INFO_CREATOR },
        { "producer", FZ_META_INFO_PRODUCER },
        { "creationDate", FZ_META_INFO_CREATIONDATE },
        { "modificationDate", FZ_META_INFO_MODIFICATIONDATE },
        { "format", FZ_META_FORMAT },
        { "security", FZ_META_ENCRYPTION },
    } };

    const auto wanted = [&keys](const char* name) {
        return keys.empty() || std::find(keys.begin(), keys.end(), name) != keys.end();
    };

    // Engine-common values: the MuPDF version compiled into this worker binary
    // is authoritative for the generator, and the runtime records whether the
    // open required a non-empty password. Raw Info-dictionary date strings are
    // reported as-is; the generator layer owns user-facing formatting. Common
    // values are excluded from the metadata hash, which covers document
    // identity only.
    if (wanted("engineVersion"))
        result.values.emplace("engineVersion", FZ_VERSION);
    if (wanted("documentHasPassword"))
        result.values.emplace("documentHasPassword", m_passwordRequired ? "true" : "false");

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
        if (!wanted(name))
            continue;

        // Date fields are normalized to ISO 8601 UTC so the generator can
        // parse them with QDateTime::fromString(..., Qt::ISODate). The hash
        // above still covers the raw Info bytes as document identity.
        if (name == "creationDate" || name == "modificationDate") {
            std::string iso = toIsoTimestamp(m_context, value);
            if (!iso.empty())
                result.values.emplace(name, std::move(iso));
        } else {
            result.values.emplace(name, std::move(value));
        }
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
