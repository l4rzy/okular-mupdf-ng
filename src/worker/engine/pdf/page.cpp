// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

namespace {

/// Determines whether a destination type supplies an X or Y target coordinate.
bool hasDestinationCoordinate(fz_link_dest_type type, bool x)
{
    switch (type) {
    case FZ_LINK_DEST_FIT_H:
    case FZ_LINK_DEST_FIT_BH:
        return !x;
    case FZ_LINK_DEST_FIT_V:
    case FZ_LINK_DEST_FIT_BV:
        return x;
    case FZ_LINK_DEST_FIT_R:
    case FZ_LINK_DEST_XYZ:
        return true;
    default:
        return false;
    }
}

/// Normalizes raw destination point coordinates into normalized [0.0, 1.0] viewport fractions.
std::uint8_t
normalizeDestination(const fz_link_dest& destination, fz_rect cropBox, fz_matrix pageCtm, Viewport& viewport)
{
    const bool hasX = hasDestinationCoordinate(destination.type, true) && std::isfinite(destination.x);
    const bool hasY = hasDestinationCoordinate(destination.type, false) && std::isfinite(destination.y);
    if (!hasX && !hasY)
        return 0;

    // fz_resolve_link_dest() returns coordinates in MuPDF's top-left page
    // coordinate system. Named destinations are transformed while resolving;
    // explicit URI destinations already use the same convention.
    const fz_point pagePoint { hasX ? destination.x : 0, hasY ? destination.y : 0 };
    const fz_rect pageBounds = fz_transform_rect(cropBox, pageCtm);
    const float width = pageBounds.x1 - pageBounds.x0;
    const float height = pageBounds.y1 - pageBounds.y0;
    if (!(width > 0 && height > 0) || !std::isfinite(pagePoint.x) || !std::isfinite(pagePoint.y))
        return 0;

    std::uint8_t coordinateMask = 0;
    if (hasX) {
        viewport.normalizedX = (pagePoint.x - pageBounds.x0) / width;
        coordinateMask |= Viewport::CoordinateX;
    }
    if (hasY) {
        viewport.normalizedY = (pagePoint.y - pageBounds.y0) / height;
        if (height > Constant::DestinationTopMarginPoints)
            viewport.normalizedY = std::max(0.0, viewport.normalizedY - Constant::DestinationTopMarginPoints / height);
        coordinateMask |= Viewport::CoordinateY;
    }
    return coordinateMask;
}

} // namespace

// =============================================================================
// Page Geometry Queries
// =============================================================================

PageGeometry PdfDocument::pageGeometry(int page, std::string* error) const
{
    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return { };

    PageGeometry geometry;
    fz_try(m_context)
    {
        geometry = geometryFromPage(nativePage, fz_bound_page(m_context, nativePage));
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
    return geometry;
}

PageGeometry PdfDocument::geometryFromPage(fz_page* page, const fz_rect& bounds) const
{
    char label[64] { };
    if (page)
        fz_page_label(m_context, page, label, sizeof(label));
    return { bounds.x1 - bounds.x0, bounds.y1 - bounds.y0, -1.0, label };
}

// =============================================================================
// Annotation Extraction & Coordinate Normalization
// =============================================================================

std::vector<Annotation> PdfDocument::extractAnnotations(int page, std::string* error) const
{
    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return { };

    std::vector<Annotation> result;
    fz_try(m_context)
    {
        const fz_rect bounds = fz_bound_page(m_context, nativePage);
        result = extractPageAnnotations(nativePage, bounds, error);
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
    return result;
}

// =============================================================================
// Link Extraction & Destination Resolution
// =============================================================================

ResolvedLink PdfDocument::resolveLink(const std::string& uri, std::string* error) const
{
    ResolvedLink result;
    if (!m_document || m_locked || uri.empty()) {
        if (!m_document || m_locked)
            fail(error, "document is unavailable");
        return result;
    }

    // Check the temporary cache populated while the worker incrementally resolves
    // page links after the initial document response has been sent.
    if (m_resolvedLinkCacheEnabled) {
        if (const auto cached = m_resolvedLinks.find(uri); cached != m_resolvedLinks.end())
            return cached->second;
    }

    fz_try(m_context)
    {
        if (fz_is_external_link(m_context, uri.c_str())) {
            // External URI link (e.g. http://, mailto:)
            result.external = true;
            result.uri = uri;
            result.valid = true;
        } else {
            // Internal PDF page destination (e.g. #page=5, named destinations)
            const fz_link_dest destination = fz_resolve_link_dest(m_context, m_document, uri.c_str());
            if (destination.loc.page >= 0 && destination.loc.page < m_pageCount) {
                result.viewport.page = destination.loc.page;
                result.viewport.coordinateMask = 0;
                if (pdf_document* pdfDocument = pdf_specifics(m_context, m_document)) {
                    fz_try(m_context)
                    {
                        // Transform destination points into the target page coordinate system
                        pdf_obj* pageObject = pdf_lookup_page_obj(m_context, pdfDocument, destination.loc.page);
                        if (pageObject) {
                            fz_rect cropBox { };
                            fz_matrix pageCtm { };
                            pdf_page_obj_transform(m_context, pageObject, &cropBox, &pageCtm);
                            result.viewport.coordinateMask =
                                normalizeDestination(destination, cropBox, pageCtm, result.viewport);
                        }
                    }
                    fz_catch(m_context)
                    {
                        // A malformed page object must not invalidate an
                        // otherwise valid page destination.
                        fz_ignore_error(m_context);
                    }
                }
                result.valid = true;
            }
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }

    // Cache only internal destinations: external URLs do not require page
    // coordinate resolution and the cache is discarded after aggregation.
    if (m_resolvedLinkCacheEnabled && result.valid && !result.external
        && m_resolvedLinks.size() < Constant::MaxResolvedLinkCacheEntries
        && uri.size() <= Constant::MaxResolvedLinkCacheKeyBytes - m_resolvedLinkKeyBytes) {
        const auto [_, inserted] = m_resolvedLinks.emplace(uri, result);
        if (inserted)
            m_resolvedLinkKeyBytes += uri.size();
    }

    return result;
}

std::vector<Link> PdfDocument::extractLinks(int page, std::string* error) const
{
    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return { };

    std::vector<Link> result;
    fz_try(m_context)
    {
        result = extractPageLinks(nativePage, fz_bound_page(m_context, nativePage), error);
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
    return result;
}

std::vector<Link> PdfDocument::extractPageLinks(fz_page* nativePage, const fz_rect& bounds, std::string* error) const
{
    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    if (width <= 0 || height <= 0)
        fz_throw(m_context, FZ_ERROR_GENERIC, "page has invalid bounds");

    fz_link* list = nullptr;
    std::vector<Link> result;
    fz_var(list);
    fz_try(m_context)
    {
        // Load link annotations from page content stream and annotation dictionaries
        list = fz_load_links(m_context, nativePage);
        for (fz_link* link = list; link; link = link->next) {
            if (result.size() >= Constant::MaxPageLinks)
                fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: page link limit exceeded");

            if (!link->uri)
                continue;

            Link value;
            value.left = (link->rect.x0 - bounds.x0) / width;
            value.top = (link->rect.y0 - bounds.y0) / height;
            value.right = (link->rect.x1 - bounds.x0) / width;
            value.bottom = (link->rect.y1 - bounds.y0) / height;
            value.target = resolveLink(link->uri, error);
            if (value.target.valid)
                result.push_back(std::move(value));
        }
    }
    fz_always(m_context)
    {
        if (list)
            fz_drop_link(m_context, list);
    }
    fz_catch(m_context)
    {
        fz_rethrow(m_context);
    }
    return result;
}

// =============================================================================
// Combined Page Details (Single-Pass Geometry, Annotations, Links, Signatures)
// =============================================================================

DocumentBase::PageDetails PdfDocument::pageDetails(int page, std::string* error, bool includeLinks) const
{
    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return { };

    PageDetails result;
    fz_try(m_context)
    {
        const fz_rect bounds = fz_bound_page(m_context, nativePage);
        result.geometry = geometryFromPage(nativePage, bounds);
        result.annotations = extractPageAnnotations(nativePage, bounds, error);
        if (error && !error->empty())
            fz_throw(m_context, FZ_ERROR_GENERIC, "annotation extraction failed: %s", error->c_str());
        result.signatures = extractPageSignatures(nativePage, bounds);
        if (includeLinks)
            result.links = extractPageLinks(nativePage, bounds, error);
        if (error && !error->empty())
            fz_throw(m_context, FZ_ERROR_GENERIC, "link extraction failed: %s", error->c_str());
        result.formFields = extractPageFormFields(nativePage, bounds, page, error);
        if (error && !error->empty())
            fz_throw(m_context, FZ_ERROR_GENERIC, "form extraction failed: %s", error->c_str());
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
    return result;
}

} // namespace Mu::Worker::Engine
