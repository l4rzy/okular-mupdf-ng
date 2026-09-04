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

    // Enforce depth and node limits without throwing: fz_try uses
    // setjmp/longjmp, so no C++ object with a destructor may live across it.
    if (depth > Constant::MaxOutlineDepth) {
        fail(error, "resource limit: outline nesting limit exceeded");
        return { };
    }
    if (count && *count >= Constant::MaxEpubOutlineNodes) {
        fail(error, "resource limit: outline node limit exceeded");
        return { };
    }

    std::vector<OutlineNode> result;
    for (const fz_outline* item = source; item; item = item->next) {
        if (count && ++(*count) > Constant::MaxEpubOutlineNodes) {
            fail(error, "resource limit: outline node limit exceeded");
            return { };
        }

        OutlineNode node;
        if (item->title)
            node.title = item->title;
        node.open = item->is_open != 0;

        if (item->uri) {
            // resolveLink contains its own narrow fz_try/catch and never throws.
            node.link = resolveLink(item->uri, nullptr);
        } else {
            // Isolate the only throwing MuPDF call so no C++ object is live
            // across the longjmp; a bad destination only leaves this link invalid.
            int pageNum = -1;
            fz_var(pageNum);
            fz_try(m_context)
            {
                pageNum = fz_page_number_from_location(m_context, m_document, item->page);
            }
            fz_catch(m_context)
            {
                pageNum = -1;
            }
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
