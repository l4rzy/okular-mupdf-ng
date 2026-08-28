// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/constants.hpp"
#include "engine/epub/document.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Outline / Table-of-Contents Extraction
// =============================================================================

std::vector<OutlineNode>
EpubDocument::copyOutline(const fz_outline* source, std::size_t depth, std::size_t* count, std::string* error) const
{
    if (!source)
        return { };

    std::vector<OutlineNode> result;
    OutlineNode node;
    fz_try(m_context)
    {
        // Enforce depth and node limits inside the protected MuPDF boundary.
        if (depth > Constant::MaxOutlineDepth)
            fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: outline nesting limit exceeded");
        if (count && *count >= Constant::MaxEpubOutlineNodes)
            fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: outline node limit exceeded");

        for (const fz_outline* item = source; item; item = item->next) {
            if (count && ++(*count) > Constant::MaxEpubOutlineNodes)
                fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: outline node limit exceeded");
            node = { };
            if (item->title)
                node.title = item->title;
            node.open = item->is_open != 0;

            if (item->uri) {
                node.link = resolveLink(item->uri, nullptr);
            } else {
                const int pageNum = fz_page_number_from_location(m_context, m_document, item->page);
                if (pageNum >= 0) {
                    node.link.viewport.page = pageNum;
                    node.link.valid = true;
                }
            }

            // Recurse into child items
            if (item->down) {
                node.children = copyOutline(item->down, depth + 1, count, error);
                if (error && !error->empty())
                    return { };
            }

            result.push_back(std::move(node));
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    return result;
}

std::vector<OutlineNode> EpubDocument::outline(std::string* error) const
{
    if (!m_document) {
        fail(error, "document is unavailable");
        return { };
    }
    std::vector<OutlineNode> result;
    fz_outline* outlineHead = nullptr;
    fz_var(outlineHead);
    fz_try(m_context)
    {
        // Load table-of-contents tree from EPUB navigation document / NCX
        outlineHead = fz_load_outline(m_context, m_document);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return result;
    }

    if (outlineHead) {
        try {
            std::size_t count = 0;
            std::string outlineError;
            result = copyOutline(outlineHead, 0, &count, &outlineError);
            if (!outlineError.empty()) {
                fail(error, outlineError);
                result.clear();
            }
        } catch (...) {
            fz_drop_outline(m_context, outlineHead);
            throw;
        }
        fz_drop_outline(m_context, outlineHead);
    }
    return result;
}

} // namespace Mu::Worker::Engine
