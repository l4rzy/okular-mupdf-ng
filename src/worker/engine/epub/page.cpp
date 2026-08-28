// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/constants.hpp"
#include "engine/epub/document.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Page Geometry & Details
// =============================================================================

PageGeometry EpubDocument::pageGeometry(int page, std::string* error) const
{
    if (!m_document || page < 0 || page >= m_pageCount) {
        fail(error, "page is unavailable");
        return { };
    }

    const auto layout = layoutGeometry();
    return { layout.paperWidth, layout.paperHeight, -1.0, std::to_string(page + 1) };
}

std::vector<Annotation> EpubDocument::extractAnnotations(int, std::string*) const
{
    // EPUB documents do not support PDF-style native annotations
    return { };
}

DocumentBase::PageDetails EpubDocument::pageDetails(int page, std::string* error, bool includeLinks) const
{
    if (!includeLinks) {
        PageDetails details;
        details.geometry = pageGeometry(page, error);
        return details;
    }

    fz_rect bounds { };
    fz_page* pagePtr = loadPageWithBounds(page, &bounds, error);
    if (!pagePtr)
        return { };

    PageDetails details;
    fz_try(m_context)
    {
        char labelBuf[64] { };
        fz_page_label(m_context, pagePtr, labelBuf, sizeof(labelBuf));
        const auto layout = layoutGeometry();
        details.geometry = {
            layout.paperWidth, layout.paperHeight, -1.0, labelBuf[0] ? std::string(labelBuf) : std::to_string(page + 1)
        };
        if (includeLinks)
            details.links = extractPageLinks(pagePtr, bounds, error);
    }
    fz_always(m_context)
    {
        fz_drop_page(m_context, pagePtr);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    return details;
}

// =============================================================================
// Link Extraction & Destination Resolution
// =============================================================================

ResolvedLink EpubDocument::resolveLink(const std::string& uri, std::string* error) const
{
    ResolvedLink result;
    if (!m_document || uri.empty()) {
        if (!m_document)
            fail(error, "document is unavailable");
        return result;
    }
    fz_try(m_context)
    {
        if (fz_is_external_link(m_context, uri.c_str())) {
            // External URI link (e.g. http://, mailto:)
            result.external = true;
            result.uri = uri;
            result.valid = true;
        } else {
            // Internal EPUB HTML anchor destination resolved to layout page location
            const fz_link_dest destination = fz_resolve_link_dest(m_context, m_document, uri.c_str());
            const int pageNum = fz_page_number_from_location(m_context, m_document, destination.loc);
            if (pageNum >= 0 && pageNum < m_pageCount) {
                result.viewport.page = pageNum;
                result.valid = true;
            }
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
    }
    return result;
}

std::vector<Link> EpubDocument::extractLinks(int page, std::string* error) const
{
    fz_rect bounds { };
    fz_page* pagePtr = loadPageWithBounds(page, &bounds, error);
    if (!pagePtr)
        return { };

    std::vector<Link> result;
    fz_try(m_context)
    {
        result = extractPageLinks(pagePtr, bounds, error);
    }
    fz_always(m_context)
    {
        fz_drop_page(m_context, pagePtr);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    return result;
}

std::vector<Link> EpubDocument::extractPageLinks(fz_page* pagePtr, const fz_rect& bounds, std::string* error) const
{
    std::vector<Link> result;
    fz_link* linkList = nullptr;
    fz_var(linkList);
    fz_try(m_context)
    {
        const float width = bounds.x1 - bounds.x0;
        const float height = bounds.y1 - bounds.y0;
        if (width <= 0 || height <= 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "page has invalid bounds");
        linkList = fz_load_links(m_context, pagePtr);

        for (fz_link* link = linkList; link; link = link->next) {
            if (result.size() >= Constant::MaxPageLinks)
                fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: page link limit exceeded");
            const double left = (link->rect.x0 - bounds.x0) / width;
            const double top = (link->rect.y0 - bounds.y0) / height;
            const double right = (link->rect.x1 - bounds.x0) / width;
            const double bottom = (link->rect.y1 - bounds.y0) / height;

            ResolvedLink target;
            if (link->uri)
                target = resolveLink(link->uri, nullptr);
            result.push_back({ left, top, right, bottom, std::move(target) });
        }
    }
    fz_always(m_context)
    {
        if (linkList)
            fz_drop_link(m_context, linkList);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    return result;
}

} // namespace Mu::Worker::Engine
