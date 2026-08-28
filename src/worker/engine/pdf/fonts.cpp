// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Embedded Font Enumeration
// =============================================================================

std::vector<Font> PdfDocument::fonts(const std::vector<int>& pages, std::string* error) const
{
    if (!m_document || m_locked) {
        fail(error, "document is unavailable");
        return { };
    }
    if (!pdf_specifics(m_context, m_document))
        return { };

    // A font resource can be referenced by many pages. Deduplicate by its PDF
    // name while preserving the first resource classification encountered.
    std::vector<Font> result;
    std::set<std::string, std::less<>> seen;
    fz_page* nativePage = nullptr;
    fz_try(m_context)
    {
        for (const int page : pages) {
            if (page < 0 || page >= m_pageCount)
                fz_throw(m_context, FZ_ERROR_ARGUMENT, "page is unavailable");

            nativePage = fz_load_page(m_context, m_document, page);
            fz_var(nativePage);
            fz_try(m_context)
            {
                pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
                pdf_obj* resources = pdfPage ? pdf_page_resources(m_context, pdfPage) : nullptr;
                pdf_obj* fonts = resources ? pdf_dict_get(m_context, resources, PDF_NAME(Font)) : nullptr;
                const int fontCount = fonts ? pdf_dict_len(m_context, fonts) : 0;

                // Inspect each font resource attached to the page
                for (int index = 0; index < fontCount; ++index) {
                    pdf_obj* font = pdf_dict_get_val(m_context, fonts, index);
                    if (!font)
                        continue;

                    const char* name = pdf_to_name(m_context, pdf_dict_gets(m_context, font, "BaseFont"));
                    if (!name || !*name)
                        name = pdf_to_name(m_context, pdf_dict_gets(m_context, font, "Name"));
                    if (!name || !*name || !seen.emplace(name).second)
                        continue;

                    const char* subtype = pdf_to_name(m_context, pdf_dict_gets(m_context, font, "Subtype"));
                    pdf_obj* descriptor = pdf_dict_gets(m_context, font, "FontDescriptor");

                    // For composite Type0 fonts, inspect descendant CIDFont dictionary
                    if (subtype && std::strcmp(subtype, "Type0") == 0) {
                        pdf_obj* descendants = pdf_dict_gets(m_context, font, "DescendantFonts");
                        if (descendants && pdf_array_len(m_context, descendants) > 0) {
                            pdf_obj* descendant = pdf_array_get(m_context, descendants, 0);
                            if (descendant) {
                                if (pdf_obj* value = pdf_dict_gets(m_context, descendant, "FontDescriptor"))
                                    descriptor = value;
                                if (const char* value =
                                        pdf_to_name(m_context, pdf_dict_gets(m_context, descendant, "Subtype"));
                                    value && *value)
                                    subtype = value;
                            }
                        }
                    }

                    // Check for embedded font streams (/FontFile, /FontFile2,
                    // /FontFile3); Type3 fonts are defined by page content and
                    // are treated as embedded even without a descriptor stream.
                    const bool embedded = (descriptor
                                           && (pdf_dict_gets(m_context, descriptor, "FontFile")
                                               || pdf_dict_gets(m_context, descriptor, "FontFile2")
                                               || pdf_dict_gets(m_context, descriptor, "FontFile3")))
                        || (subtype && std::strcmp(subtype, "Type3") == 0);

                    std::string fontName(name);
                    // Detect standard 6-character uppercase subset tag prefix (e.g. ABCDEF+FontName)
                    const bool subset = fontName.size() > 7 && fontName[6] == '+'
                        && std::all_of(fontName.begin(), fontName.begin() + 6, [](char character) {
                                            return character >= 'A' && character <= 'Z';
                                        });

                    result.push_back({ std::move(fontName),
                                       { },
                                       mapFontType(subtype),
                                       embedded
                                           ? (subset ? FontEmbedType::EmbeddedSubset : FontEmbedType::FullyEmbedded)
                                           : FontEmbedType::NotEmbedded });
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
// Font Type Mapping Helper
// =============================================================================

Model::FontType PdfDocument::mapFontType(const char* subtype) noexcept
{
    if (!subtype)
        return Model::FontType::Unknown;
    if (std::strcmp(subtype, "Type1") == 0)
        return Model::FontType::Type1;
    if (std::strcmp(subtype, "Type1C") == 0 || std::strcmp(subtype, "MMType1") == 0)
        return Model::FontType::Type1C;
    if (std::strcmp(subtype, "Type3") == 0)
        return Model::FontType::Type3;
    if (std::strcmp(subtype, "TrueType") == 0)
        return Model::FontType::TrueType;
    if (std::strcmp(subtype, "OpenType") == 0)
        return Model::FontType::TrueTypeOT;
    if (std::strcmp(subtype, "CIDFontType0") == 0 || std::strcmp(subtype, "CIDFontType0C") == 0)
        return Model::FontType::CIDType0;
    if (std::strcmp(subtype, "CIDFontType2") == 0)
        return Model::FontType::CIDTrueType;
    return Model::FontType::Unknown;
}

} // namespace Mu::Worker::Engine
