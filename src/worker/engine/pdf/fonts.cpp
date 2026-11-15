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
    pdf_document* pdf = pdf_specifics(m_context, m_document);
    if (!pdf)
        return { };

    // A font resource can be referenced by many pages. Deduplicate by its PDF
    // name while preserving the first resource classification encountered.
    std::vector<Font> result;
    std::set<std::string, std::less<>> seen;
    std::set<pdf_obj*> visitedResources;
    std::vector<pdf_obj*> pendingResources;
    // Resolve and deduplicate before queuing so aliases cannot inflate the worklist.
    const auto enqueueResources = [&](pdf_obj* resources) {
        resources = pdf_resolve_indirect_chain(m_context, resources);
        if (pdf_is_dict(m_context, resources) && visitedResources.insert(resources).second)
            pendingResources.push_back(resources);
    };
    fz_try(m_context)
    {
        for (const int page : pages) {
            if (page < 0 || page >= m_pageCount)
                fz_throw(m_context, FZ_ERROR_ARGUMENT, "page is unavailable");

            pdf_obj* pageObject = pdf_lookup_page_obj(m_context, pdf, page);
            // Fonts in annotation appearances need not occur in page resources.
            pdf_obj* annots = pdf_dict_get(m_context, pageObject, PDF_NAME(Annots));
            for (int index = 0; index < pdf_array_len(m_context, annots); ++index) {
                pdf_obj* appearance = pdf_dict_get(m_context, pdf_array_get(m_context, annots, index), PDF_NAME(AP));
                for (pdf_obj* key : { PDF_NAME(N), PDF_NAME(R), PDF_NAME(D) }) {
                    pdf_obj* value = pdf_dict_get(m_context, appearance, key);
                    if (pdf_is_stream(m_context, value))
                        enqueueResources(pdf_dict_get(m_context, value, PDF_NAME(Resources)));
                    else {
                        for (int state = 0; state < pdf_dict_len(m_context, value); ++state)
                            enqueueResources(pdf_dict_get(
                                m_context, pdf_dict_get_val(m_context, value, state), PDF_NAME(Resources)));
                    }
                }
            }
            enqueueResources(pdf_dict_get_inheritable(m_context, pageObject, PDF_NAME(Resources)));
            // Walk resource dictionaries iteratively: shared and cyclic Form
            // XObjects must neither hide fonts nor cause unbounded recursion.
            while (!pendingResources.empty()) {
                pdf_obj* resources = pendingResources.back();
                pendingResources.pop_back();
                pdf_obj* fonts = pdf_dict_get(m_context, resources, PDF_NAME(Font));
                const int fontCount = pdf_dict_len(m_context, fonts);
                // Inspect fonts in this resource dictionary.
                for (int index = 0; index < fontCount; ++index) {
                    pdf_obj* font = pdf_dict_get_val(m_context, fonts, index);
                    if (!font)
                        continue;

                    enqueueResources(pdf_dict_get(m_context, font, PDF_NAME(Resources)));
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
                for (pdf_obj* key : { PDF_NAME(XObject), PDF_NAME(Pattern) }) {
                    pdf_obj* objects = pdf_dict_get(m_context, resources, key);
                    for (int index = 0; index < pdf_dict_len(m_context, objects); ++index)
                        enqueueResources(
                            pdf_dict_get(m_context, pdf_dict_get_val(m_context, objects, index), PDF_NAME(Resources)));
                }
                pdf_obj* graphicsStates = pdf_dict_get(m_context, resources, PDF_NAME(ExtGState));
                for (int index = 0; index < pdf_dict_len(m_context, graphicsStates); ++index)
                    enqueueResources(pdf_dict_getp(
                        m_context, pdf_dict_get_val(m_context, graphicsStates, index), "SMask/G/Resources"));
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
