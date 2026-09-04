// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/constants.hpp"
#include "engine/epub/document.hpp"

#include <cmath>

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

    // Only the POD label buffer lives across the protected call; all C++
    // construction happens after fz_catch so no destructor is skipped by longjmp.
    char labelBuf[64] { };
    fz_try(m_context)
    {
        fz_page_label(m_context, pagePtr, labelBuf, sizeof(labelBuf));
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        fz_try(m_context)
        {
            fz_drop_page(m_context, pagePtr);
        }
        fz_catch(m_context)
        {
        }
        return { };
    }

    PageDetails details;
    const auto layout = layoutGeometry();
    details.geometry = {
        layout.paperWidth, layout.paperHeight, -1.0, labelBuf[0] ? std::string(labelBuf) : std::to_string(page + 1)
    };
    // extractPageLinks never throws across C++: it reports via error only.
    if (includeLinks)
        details.links = extractPageLinks(pagePtr, bounds, error);

    fz_try(m_context)
    {
        fz_drop_page(m_context, pagePtr);
    }
    fz_catch(m_context)
    {
    }
    if (error && !error->empty())
        return { };
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
    // Snapshot the MuPDF outcome into PODs only; the C++ result is built
    // after fz_catch so no std::string lives across the longjmp.
    bool isExternal = false;
    bool found = false;
    int pageNum = -1;
    fz_var(isExternal);
    fz_var(found);
    fz_var(pageNum);
    fz_try(m_context)
    {
        if (fz_is_external_link(m_context, uri.c_str())) {
            isExternal = true;
        } else {
            // Internal EPUB HTML anchor destination resolved to layout page location
            const fz_link_dest destination = fz_resolve_link_dest(m_context, m_document, uri.c_str());
            pageNum = fz_page_number_from_location(m_context, m_document, destination.loc);
            found = (pageNum >= 0 && pageNum < m_pageCount);
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    if (isExternal) {
        // External URI link (e.g. http://, mailto:)
        result.external = true;
        result.uri = uri;
        result.valid = true;
    } else if (found) {
        result.viewport.page = pageNum;
        result.valid = true;
    }
    return result;
}

std::vector<Link> EpubDocument::extractLinks(int page, std::string* error) const
{
    fz_rect bounds { };
    fz_page* pagePtr = loadPageWithBounds(page, &bounds, error);
    if (!pagePtr)
        return { };

    // extractPageLinks reports via error and never throws across C++.
    std::vector<Link> result = extractPageLinks(pagePtr, bounds, error);
    fz_try(m_context)
    {
        fz_drop_page(m_context, pagePtr);
    }
    fz_catch(m_context)
    {
    }
    if (error && !error->empty())
        return { };
    return result;
}

std::vector<Link> EpubDocument::extractPageLinks(fz_page* pagePtr, const fz_rect& bounds, std::string* error) const
{
    std::vector<Link> result;
    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    if (!(width > 0 && height > 0)) {
        fail(error, "page has invalid bounds");
        return result;
    }

    // Load the C link list in a narrow protected region; iteration and all
    // C++ work happen outside so no destructor is skipped by longjmp.
    fz_link* linkList = nullptr;
    fz_var(linkList);
    fz_try(m_context)
    {
        linkList = fz_load_links(m_context, pagePtr);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return result;
    }

    std::size_t count = 0;
    for (fz_link* link = linkList; link; link = link->next) {
        if (++count > Constant::MaxPageLinks) {
            fail(error, "resource limit: page link limit exceeded");
            result.clear();
            break;
        }
        const double left = (link->rect.x0 - bounds.x0) / width;
        const double top = (link->rect.y0 - bounds.y0) / height;
        const double right = (link->rect.x1 - bounds.x0) / width;
        const double bottom = (link->rect.y1 - bounds.y0) / height;
        if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom))
            continue;

        // resolveLink never throws across C++; the raw uri pointer stays
        // valid until the list is dropped below.
        ResolvedLink target;
        if (link->uri)
            target = resolveLink(link->uri, nullptr);
        result.push_back({ left, top, right, bottom, std::move(target) });
    }

    fz_try(m_context)
    {
        if (linkList)
            fz_drop_link(m_context, linkList);
    }
    fz_catch(m_context)
    {
        fz_ignore_error(m_context);
    }
    return result;
}

} // namespace Mu::Worker::Engine
