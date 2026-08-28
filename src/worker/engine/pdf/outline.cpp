// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <cstddef>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Outline / Table-of-Contents Extraction
// =============================================================================

std::vector<OutlineNode> PdfDocument::outline(std::string* error) const
{
    if (!m_document || m_locked) {
        fail(error, "document is unavailable");
        return { };
    }

    fz_outline* root = nullptr;
    std::vector<OutlineNode> result;

    fz_var(root);
    fz_try(m_context)
    {
        // Load hierarchical document outline from Fitz engine
        root = fz_load_outline(m_context, m_document);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }

    std::size_t count = 0;
    try {
        result = copyOutline(root, 0, &count, error);
    } catch (...) {
        if (root)
            fz_drop_outline(m_context, root);
        throw;
    }
    if (root)
        fz_drop_outline(m_context, root);
    if (error && !error->empty())
        return { };

    return result;
}

std::vector<OutlineNode>
PdfDocument::copyOutline(const fz_outline* source, std::size_t depth, std::size_t* count, std::string* error) const
{
    // Enforce depth limit against maliciously circular or deeply nested outline trees
    if (depth > Constant::MaxOutlineDepth) {
        fail(error, "outline nesting limit exceeded");
        return { };
    }

    std::vector<OutlineNode> result;
    for (const fz_outline* item = source; item; item = item->next) {
        // Enforce total node count limit
        if (++*count > Constant::MaxOutlineNodes) {
            fail(error, "outline node limit exceeded");
            return { };
        }

        OutlineNode node;
        if (item->title)
            node.title = item->title;

        node.open = item->is_open != 0;
        if (item->uri)
            node.link = resolveLink(item->uri, error);
        if (error && !error->empty())
            return { };

        // Recurse into children items
        node.children = copyOutline(item->down, depth + 1, count, error);
        if (error && !error->empty())
            return { };
        result.push_back(std::move(node));
    }
    return result;
}

} // namespace Mu::Worker::Engine
