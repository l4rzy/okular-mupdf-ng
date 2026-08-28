// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Embedded Attachment Extraction
// =============================================================================

std::vector<EmbeddedFile>
PdfDocument::embeddedFiles(std::size_t maxBytes, std::size_t maxFiles, bool* resourceLimit, std::string* error) const
{
    if (resourceLimit)
        *resourceLimit = false;

    if (!m_document || m_locked) {
        fail(error, "document is unavailable");
        return { };
    }

    pdf_document* pdf = pdf_specifics(m_context, m_document);
    if (!pdf)
        return { };

    std::vector<EmbeddedFile> result;
    std::size_t remainingBytes = maxBytes;
    std::size_t remainingFiles = maxFiles;

    fz_try(m_context)
    {
        // Step 1: Collect document-level attachments from the global EmbeddedFiles name tree
        pdf_obj* trailer = pdf_trailer(m_context, pdf);
        pdf_obj* root = trailer ? pdf_dict_gets(m_context, trailer, "Root") : nullptr;
        pdf_obj* names = root ? pdf_dict_gets(m_context, root, "Names") : nullptr;
        pdf_obj* tree = names ? pdf_dict_gets(m_context, names, "EmbeddedFiles") : nullptr;

        collectEmbeddedTree(m_context, tree, result, 0, remainingBytes, remainingFiles, resourceLimit);

        // Step 2: Collect page-level file attachment annotations across all pages
        fz_page* nativePage = nullptr;
        for (int page = 0; page < m_pageCount; ++page) {
            nativePage = fz_load_page(m_context, m_document, page);
            fz_var(nativePage);
            fz_try(m_context)
            {
                pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
                for (pdf_annot* annotation = pdfPage ? pdf_first_annot(m_context, pdfPage) : nullptr; annotation;
                     annotation = pdf_next_annot(m_context, annotation)) {
                    if (pdf_annot_type(m_context, annotation) != PDF_ANNOT_FILE_ATTACHMENT)
                        continue;

                    pdf_obj* object = pdf_annot_obj(m_context, annotation);
                    pdf_obj* filespec = object ? pdf_dict_gets(m_context, object, "FS") : nullptr;
                    EmbeddedFile file = parseFilespec(m_context, filespec, remainingBytes);

                    if (file.contentTooLarge || file.data.size() > remainingBytes || remainingFiles == 0) {
                        if (resourceLimit)
                            *resourceLimit = true;
                        return { };
                    }

                    if (!file.name.empty()) {
                        remainingBytes -= file.data.size();
                        --remainingFiles;
                        result.push_back(std::move(file));
                    }
                }
            }
            fz_always(m_context)
            {
                fz_drop_page(m_context, nativePage);
                nativePage = nullptr;
            }
            fz_catch(m_context)
            {
                fz_rethrow(m_context);
            }
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }

    return result;
}

// =============================================================================
// PDF Date Parsing
// =============================================================================

// PDF dates are D:YYYYMMDDHHmmSS[tz]. Only the UTC prefix is parsed; a trailing
// timezone offset or Z is intentionally ignored and missing fields fall back to
// zero. The chrono year_month_day round-trip validates the calendar date.
Timestamp PdfDocument::parsePdfDate(fz_context* context, pdf_obj* object)
{
    if (!object || !pdf_is_string(context, object))
        return { };

    const char* text = pdf_to_str_buf(context, object);
    if (!text)
        return { };

    std::string_view value(text);
    if (value.starts_with("D:"))
        value.remove_prefix(2);

    const auto read = [&value](std::size_t offset, std::size_t length, int fallback, int* target) {
        if (value.size() < offset + length) {
            *target = fallback;
            return true;
        }
        const auto result = std::from_chars(value.data() + offset, value.data() + offset + length, *target);
        return result.ec == std::errc();
    };

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!read(0, 4, 0, &year) || !read(4, 2, 0, &month) || !read(6, 2, 0, &day) || !read(8, 2, 0, &hour)
        || !read(10, 2, 0, &minute) || !read(12, 2, 0, &second))
        return { };

    const std::chrono::year_month_day date { std::chrono::year(year),
                                             std::chrono::month(static_cast<unsigned>(month)),
                                             std::chrono::day(static_cast<unsigned>(day)) };

    if (!date.ok() || hour > 23 || minute > 59 || second > 59)
        return { };

    const auto instant = std::chrono::sys_days(date) + std::chrono::hours(hour) + std::chrono::minutes(minute)
        + std::chrono::seconds(second);
    return { true, std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count() };
}

// =============================================================================
// Filespec & Attachment Stream Parsing
// =============================================================================

// Resolves a PDF file specification (name, description, embedded stream),
// preferring the UTF-8 "UF" keys over "F". Payloads over 16 MiB are reported as
// contentTooLarge instead of being buffered, and Params/Size is cross-checked.
EmbeddedFile PdfDocument::parseFilespec(fz_context* context, pdf_obj* object, std::size_t remainingBytes)
{
    const std::size_t byteLimit = std::min(Constant::MaxEmbeddedBytes, remainingBytes);

    EmbeddedFile result;
    if (!object || !pdf_is_dict(context, object))
        return result;

    pdf_obj* filename = pdf_dict_gets(context, object, "UF");
    if (!filename)
        filename = pdf_dict_gets(context, object, "F");

    if (!filename || !pdf_is_string(context, filename))
        return result;

    if (const char* value = pdf_to_text_string(context, filename))
        result.name = value;

    if (result.name.empty())
        return result;

    if (pdf_obj* description = pdf_dict_gets(context, object, "Desc");
        description && pdf_is_string(context, description))
        if (const char* value = pdf_to_text_string(context, description))
            result.description = value;

    pdf_obj* embedded = pdf_dict_gets(context, object, "EF");
    pdf_obj* streamObject =
        embedded && pdf_is_dict(context, embedded) ? pdf_dict_gets(context, embedded, "UF") : nullptr;

    if (!streamObject && embedded && pdf_is_dict(context, embedded))
        streamObject = pdf_dict_gets(context, embedded, "F");

    if (!streamObject)
        return result;

    pdf_obj* params = pdf_dict_gets(context, streamObject, "Params");
    if (params && pdf_is_dict(context, params)) {
        if (pdf_obj* size = pdf_dict_gets(context, params, "Size"); size && pdf_is_int(context, size))
            result.size = pdf_to_int(context, size);

        result.creationDate = parsePdfDate(context, pdf_dict_gets(context, params, "CreationDate"));
        result.modificationDate = parsePdfDate(context, pdf_dict_gets(context, params, "ModDate"));
    }

    if (result.size > static_cast<std::int64_t>(byteLimit)) {
        result.contentTooLarge = true;
        return result;
    }

    // Stream raw decompressed bytes into buffer with strict resource size enforcement
    fz_stream* input = nullptr;
    fz_var(input);
    fz_try(context)
    {
        input = pdf_open_stream(context, streamObject);
        std::array<unsigned char, 64 * 1024> buffer { };
        while (input) {
            const std::size_t count = fz_read(context, input, buffer.data(), buffer.size());
            if (!count)
                break;

            if (count > byteLimit - result.data.size()) {
                result.data.clear();
                result.contentTooLarge = true;
                break;
            }
            result.data.insert(result.data.end(), buffer.begin(), buffer.begin() + count);
        }
    }
    fz_always(context)
    {
        if (input)
            fz_drop_stream(context, input);
    }
    fz_catch(context)
    {
        // Attachment parsing is best effort per filespec. Drop partial bytes
        // so a malformed stream cannot be exposed as a valid attachment.
        result.data.clear();
    }

    if (result.size <= 0 && !result.data.empty())
        result.size = static_cast<std::int64_t>(result.data.size());

    return result;
}

// =============================================================================
// Name Tree Traversal
// =============================================================================

// EmbFile tree walk. "Names" arrays hold alternating name/stream pairs, so
// entries are visited two at a time; "Kids" recurse with a depth cap to bound
// hostile trees.
void PdfDocument::collectEmbeddedTree(fz_context* context,
                                      pdf_obj* node,
                                      std::vector<EmbeddedFile>& output,
                                      int depth,
                                      std::size_t& remainingBytes,
                                      std::size_t& remainingFiles,
                                      bool* resourceLimit)
{
    if (!node || !pdf_is_dict(context, node) || depth > 32 || (resourceLimit && *resourceLimit))
        return;

    // Process leaf Names array with [key1, value1, key2, value2, ...] pairs
    if (pdf_obj* names = pdf_dict_gets(context, node, "Names"); names && pdf_is_array(context, names)) {
        const int length = pdf_array_len(context, names);
        for (int index = 1; index < length; index += 2) {
            EmbeddedFile file = parseFilespec(context, pdf_array_get(context, names, index), remainingBytes);

            if (file.contentTooLarge || file.data.size() > remainingBytes || remainingFiles == 0) {
                if (resourceLimit)
                    *resourceLimit = true;
                return;
            }

            if (!file.name.empty()) {
                remainingBytes -= file.data.size();
                --remainingFiles;
                output.push_back(std::move(file));
            }
        }
    }

    // Recursively process intermediate Kids nodes
    if (pdf_obj* kids = pdf_dict_gets(context, node, "Kids"); kids && pdf_is_array(context, kids)) {
        const int length = pdf_array_len(context, kids);
        for (int index = 0; index < length; ++index)
            collectEmbeddedTree(context,
                                pdf_array_get(context, kids, index),
                                output,
                                depth + 1,
                                remainingBytes,
                                remainingFiles,
                                resourceLimit);
    }
}

} // namespace Mu::Worker::Engine
