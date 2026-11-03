// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/constants.hpp"
#include "engine/xps/document.hpp"

#include <cmath>
#include <memory>

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Page Geometry & Details
// =============================================================================

PageGeometry XpsDocument::pageGeometry(int page, std::string* error) const
{
    fz_rect bounds { };
    fz_page* pagePtr = loadPageWithBounds(page, &bounds, error);
    if (!pagePtr)
        return { };
    fz_drop_page(m_context, pagePtr);
    return { bounds.x1 - bounds.x0, bounds.y1 - bounds.y0, -1.0, std::to_string(page + 1) };
}

std::vector<Annotation> XpsDocument::extractAnnotations(int, std::string*) const
{
    // XPS documents do not support PDF-style native annotations
    return { };
}

DocumentBase::PageDetails
XpsDocument::pageDetails(int page, std::string* error, bool includeLinks, ExtractionBudgets* budgets) const
{
    PageDetails details;
    details.geometry = pageGeometry(page, error);
    if (details.geometry.widthPoints <= 0 || details.geometry.heightPoints <= 0)
        return { };
    if (includeLinks)
        details.links = extractLinks(page, error, budgets);
    if (error && !error->empty())
        return { };
    return details;
}

// =============================================================================
// Link Extraction & Destination Resolution
// =============================================================================

ResolvedLink XpsDocument::resolveLink(const std::string& uri, std::string* error) const
{
    ResolvedLink result;
    if (!m_document || uri.empty()) {
        if (!m_document)
            fail(error, "document is unavailable");
        return result;
    }
    // Cache bounded destinations and misses for this document.
    if (const auto cached = m_resolvedLinks.find(uri); cached != m_resolvedLinks.end())
        return cached->second;
    // Snapshot the MuPDF outcome into PODs only; the C++ result is built
    // after fz_catch so no std::string lives across the longjmp.
    volatile bool isExternal = false;
    volatile bool found = false;
    volatile int pageNum = -1;
    fz_try(m_context)
    {
        if (fz_is_external_link(m_context, uri.c_str())) {
            isExternal = true;
        } else {
            // Resolve an XPS page or named destination
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
    // External URLs need no engine resolution. Transport failures never enter this cache.
    if (!result.external && m_resolvedLinks.size() < Constant::MaxResolvedLinkCacheEntries
        && uri.size() <= Constant::MaxResolvedLinkCacheKeyBytes - m_resolvedLinkKeyBytes) {
        const auto [_, inserted] = m_resolvedLinks.emplace(uri, result);
        if (inserted)
            m_resolvedLinkKeyBytes += uri.size();
    }
    return result;
}

std::vector<Link> XpsDocument::extractLinks(int page, std::string* error, ExtractionBudgets* budgets) const
{
    fz_rect bounds { };
    fz_page* pagePtr = loadPageWithBounds(page, &bounds, error);
    if (!pagePtr)
        return { };

    const auto dropPage = [this](fz_page* value) {
        fz_drop_page(m_context, value);
    };
    const std::unique_ptr<fz_page, decltype(dropPage)> owner(pagePtr, dropPage);
    ExtractionBudgets pageBudgets;
    auto result = extractPageLinks(pagePtr, bounds, error, (budgets ? *budgets : pageBudgets).links);
    if (error && !error->empty())
        return { };
    return result;
}

std::vector<Link>
XpsDocument::extractPageLinks(fz_page* pagePtr, const fz_rect& bounds, std::string* error, ByteBudget& budget) const
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
    fz_link* volatile linkList = nullptr;
    fz_try(m_context)
    {
        linkList = fz_load_links(m_context, pagePtr);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return result;
    }

    const auto dropLinks = [this](fz_link* value) {
        fz_drop_link(m_context, value);
    };
    const std::unique_ptr<fz_link, decltype(dropLinks)> owner(linkList, dropLinks);
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

        if (!budget.charge(sizeof(Link)) || !budget.chargeText(link->uri, Limit::MaxLinkStringBytes)) {
            fail(error, ExtractionBudgetError);
            result.clear();
            break;
        }

        // resolveLink never throws across C++; the raw uri pointer stays
        // valid until the list is dropped below.
        ResolvedLink target;
        if (link->uri) {
            target = resolveLink(link->uri, nullptr);
        }
        result.push_back({ left, top, right, bottom, std::move(target) });
    }

    return result;
}

} // namespace Mu::Worker::Engine
