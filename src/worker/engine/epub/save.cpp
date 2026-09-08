// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/constants.hpp"
#include "engine/epub/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unistd.h>
#include <unordered_map>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace Mu::Worker::Engine {

// =============================================================================
// EPUB-to-PDF Printing and Export
// =============================================================================

bool EpubDocument::savePdfFd(int fd, const std::vector<int>& pages, std::string* error)
{
    // The output descriptor is consumed on every path; EPUB page selection is
    // copied before the writer starts owning its output stream.
    if (fd < 0)
        return fail(error, "output FD is invalid");
    if (!m_document || !m_context) {
        ::close(fd);
        return fail(error, "document cannot be exported to PDF");
    }
    for (const int pageNumber : pages) {
        if (pageNumber < 0 || pageNumber >= m_pageCount) {
            ::close(fd);
            return fail(error, "export page is out of range");
        }
    }

    FILE* file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }

    fz_output* output = nullptr;
    fz_document_writer* writer = nullptr;
    fz_page* page = nullptr;
    fz_device* device = nullptr;
    bool saved = false;
    fz_var(output);
    fz_var(writer);
    fz_var(page);
    fz_var(saved);
    fz_var(file);

    std::vector<int> targetPages = pages;
    std::string pageError;
    if (targetPages.empty()) {
        targetPages.reserve(static_cast<std::size_t>(m_pageCount));
        for (int pageNumber = 0; pageNumber < m_pageCount; ++pageNumber)
            targetPages.push_back(pageNumber);
    }

    fz_try(m_context)
    {
        output = fz_new_output_with_file_ptr(m_context, file);

        // The writer takes ownership of output immediately, including when
        // construction throws. Do not drop output or close FILE* afterwards.
        fz_try(m_context)
        {
            writer = fz_new_pdf_writer_with_output(m_context, output, Constant::EpubPdfWriterOptions);
        }
        fz_catch(m_context)
        {
            output = nullptr;
            file = nullptr;
            fz_rethrow(m_context);
        }
        // The writer now owns both output and FILE*.
        output = nullptr;
        file = nullptr;

        const auto layout = layoutGeometry();
        if (layout.paperWidth <= 0 || layout.paperHeight <= 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "EPUB page size is invalid");

        // Write each paginated EPUB reflow page as a vector PDF page
        const fz_rect mediaBox { 0, 0, layout.paperWidth, layout.paperHeight };
        for (const int pageNumber : targetPages) {
            if (pageNumber < 0 || pageNumber >= m_pageCount)
                continue;

            fz_rect bounds { };
            pageError.clear();
            page = loadPageWithBounds(pageNumber, &bounds, &pageError);
            if (!page) {
                fz_throw(m_context,
                         FZ_ERROR_GENERIC,
                         "%s",
                         pageError.empty() ? "could not load EPUB page for PDF export" : pageError.c_str());
            }

            const float pageWidth = bounds.x1 - bounds.x0;
            const float pageHeight = bounds.y1 - bounds.y0;
            if (pageWidth <= 0 || pageHeight <= 0)
                fz_throw(m_context, FZ_ERROR_GENERIC, "EPUB page bounds are invalid");

            const fz_matrix transform =
                fz_concat(fz_translate(-bounds.x0, -bounds.y0),
                          fz_scale(layout.paperWidth / pageWidth, layout.paperHeight / pageHeight));
            device = fz_begin_page(m_context, writer, mediaBox);
            fz_run_page(m_context, page, device, transform, nullptr);
            fz_end_page(m_context, writer);
            device = nullptr;

            fz_drop_page(m_context, page);
            page = nullptr;
        }

        fz_close_document_writer(m_context, writer);
        fz_drop_document_writer(m_context, writer);
        writer = nullptr;
        saved = true;
    }
    fz_always(m_context)
    {
        if (page)
            fz_drop_page(m_context, page);
    }
    fz_catch(m_context)
    {
        // If writer construction succeeded, dropping it also closes its output;
        // otherwise output/file still need the direct cleanup below.
        if (writer) {
            fz_drop_document_writer(m_context, writer);
            writer = nullptr;
        }
        if (output)
            fz_drop_output(m_context, output);
        if (file)
            ::fclose(file);
        fail(error, fz_caught_message(m_context));
    }

    return saved;
}

bool EpubDocument::exportPdfFd(int fd, const std::vector<int>& pages, std::string* error)
{
    if (fd < 0)
        return fail(error, "output FD is invalid");
    if (!m_document || !m_context) {
        ::close(fd);
        return fail(error, "document cannot be exported to PDF");
    }
    for (const int pageNumber : pages) {
        if (pageNumber < 0 || pageNumber >= m_pageCount) {
            ::close(fd);
            return fail(error, "export page is out of range");
        }
    }

    FILE* file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }

    std::vector<int> targetPages = pages;
    if (targetPages.empty()) {
        targetPages.reserve(static_cast<std::size_t>(m_pageCount));
        for (int pageNumber = 0; pageNumber < m_pageCount; ++pageNumber)
            targetPages.push_back(pageNumber);
    }

    // Map source EPUB pages to their first destination occurrence. This keeps
    // internal links valid even when a caller supplies a page subset.
    std::unordered_map<int, int> destinationPages;
    destinationPages.reserve(targetPages.size());
    for (std::size_t index = 0; index < targetPages.size(); ++index)
        destinationPages.emplace(targetPages[index], static_cast<int>(index));

    fz_output* output = nullptr;
    pdf_document* destination = nullptr;
    fz_page* page = nullptr;
    pdf_page* destinationPage = nullptr;
    fz_device* device = nullptr;
    pdf_obj* resources = nullptr;
    fz_buffer* contents = nullptr;
    pdf_obj* pageObject = nullptr;
    char* generatedUri = nullptr;
    bool saved = false;
    fz_var(output);
    fz_var(destination);
    fz_var(page);
    fz_var(destinationPage);
    fz_var(device);
    fz_var(resources);
    fz_var(contents);
    fz_var(pageObject);
    fz_var(generatedUri);
    fz_var(saved);
    fz_var(file);

    std::string pageError;
    std::vector<Link> links;

    fz_try(m_context)
    {
        output = fz_new_output_with_file_ptr(m_context, file);
        destination = pdf_create_document(m_context);

        pdf_write_options options = pdf_default_write_options;
        pdf_parse_write_options(m_context, &options, Constant::EpubPdfWriterOptions);

        const auto layout = layoutGeometry();
        if (layout.paperWidth <= 0 || layout.paperHeight <= 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "EPUB page size is invalid");
        const fz_rect mediaBox { 0, 0, layout.paperWidth, layout.paperHeight };

        // Pass 1: render every requested page into the destination. Links are
        // deliberately not created here.
        for (const int pageNumber : targetPages) {
            fz_rect bounds { };
            pageError.clear();
            page = loadPageWithBounds(pageNumber, &bounds, &pageError);
            if (!page) {
                fz_throw(m_context,
                         FZ_ERROR_GENERIC,
                         "%s",
                         pageError.empty() ? "could not load EPUB page for PDF export" : pageError.c_str());
            }

            const float pageWidth = bounds.x1 - bounds.x0;
            const float pageHeight = bounds.y1 - bounds.y0;
            if (pageWidth <= 0 || pageHeight <= 0)
                fz_throw(m_context, FZ_ERROR_GENERIC, "EPUB page bounds are invalid");

            const fz_matrix transform =
                fz_concat(fz_translate(-bounds.x0, -bounds.y0),
                          fz_scale(layout.paperWidth / pageWidth, layout.paperHeight / pageHeight));

            device = pdf_page_write(m_context, destination, mediaBox, &resources, &contents);
            fz_run_page(m_context, page, device, transform, nullptr);
            fz_close_device(m_context, device);
            fz_drop_device(m_context, device);
            device = nullptr;
            fz_drop_page(m_context, page);
            page = nullptr;

            pageObject = pdf_add_page(m_context, destination, mediaBox, 0, resources, contents);
            pdf_insert_page(m_context, destination, -1, pageObject);
            pdf_drop_obj(m_context, pageObject);
            pageObject = nullptr;
            pdf_drop_obj(m_context, resources);
            resources = nullptr;
            fz_drop_buffer(m_context, contents);
            contents = nullptr;
        }

        // Pass 2: create links. pdf_create_link resolves internal destinations
        // against the destination page tree immediately, so a forward link
        // would fail while pages are still being inserted; all pages must
        // exist before any link is created.
        const float quietNaN = std::numeric_limits<float>::quiet_NaN();
        int destinationIndex = 0;
        for (const int pageNumber : targetPages) {
            fz_rect bounds { };
            pageError.clear();
            page = loadPageWithBounds(pageNumber, &bounds, &pageError);
            if (!page) {
                // The destination page still exists; keep indexes aligned and
                // skip only its links.
                ++destinationIndex;
                continue;
            }

            // Link extraction is intentionally best-effort: malformed source
            // link data must not make the visible page fail to export.
            links = extractPageLinks(page, bounds, &pageError);
            fz_drop_page(m_context, page);
            page = nullptr;
            if (!pageError.empty())
                links.clear();

            destinationPage = pdf_load_page(m_context, destination, destinationIndex);
            for (const auto& link : links) {
                if (!link.target.valid)
                    continue;

                fz_rect linkRect {
                    static_cast<float>(link.left * layout.paperWidth),
                    static_cast<float>(link.top * layout.paperHeight),
                    static_cast<float>(link.right * layout.paperWidth),
                    static_cast<float>(link.bottom * layout.paperHeight),
                };
                if (!std::isfinite(linkRect.x0) || !std::isfinite(linkRect.y0) || !std::isfinite(linkRect.x1)
                    || !std::isfinite(linkRect.y1))
                    continue;
                linkRect.x0 = std::clamp(linkRect.x0, 0.0f, layout.paperWidth);
                linkRect.y0 = std::clamp(linkRect.y0, 0.0f, layout.paperHeight);
                linkRect.x1 = std::clamp(linkRect.x1, 0.0f, layout.paperWidth);
                linkRect.y1 = std::clamp(linkRect.y1, 0.0f, layout.paperHeight);
                if (linkRect.x1 <= linkRect.x0 || linkRect.y1 <= linkRect.y0)
                    continue;

                const char* uri = nullptr;
                if (link.target.external) {
                    if (link.target.uri.empty())
                        continue;
                    uri = link.target.uri.c_str();
                } else {
                    const auto target = destinationPages.find(link.target.viewport.page);
                    if (target == destinationPages.end())
                        continue;
                    const fz_link_dest destinationUri =
                        fz_make_link_dest_xyz(0, target->second, quietNaN, quietNaN, quietNaN);
                    generatedUri = pdf_new_uri_from_explicit_dest(m_context, destinationUri);
                    uri = generatedUri;
                }

                pdf_create_link(m_context, destinationPage, linkRect, uri);
                if (generatedUri) {
                    fz_free(m_context, generatedUri);
                    generatedUri = nullptr;
                }
            }

            pdf_drop_page(m_context, destinationPage);
            destinationPage = nullptr;
            ++destinationIndex;
        }

        pdf_update_open_pages(m_context, destination);
        pdf_write_document(m_context, destination, output, &options);
        fz_flush_output(m_context, output);
        if (::fflush(file) != 0 || ::fsync(::fileno(file)) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "failed to flush export output");

        fz_close_output(m_context, output);
        fz_drop_output(m_context, output);
        output = nullptr;
        file = nullptr;
        saved = true;
    }
    fz_always(m_context)
    {
        if (generatedUri)
            fz_free(m_context, generatedUri);
        if (destinationPage)
            pdf_drop_page(m_context, destinationPage);
        if (page)
            fz_drop_page(m_context, page);
        if (device)
            fz_drop_device(m_context, device);
        if (pageObject)
            pdf_drop_obj(m_context, pageObject);
        if (resources)
            pdf_drop_obj(m_context, resources);
        if (contents)
            fz_drop_buffer(m_context, contents);
        if (destination)
            pdf_drop_document(m_context, destination);
    }
    fz_catch(m_context)
    {
        if (output) {
            fz_try(m_context)
            {
                fz_close_output(m_context, output);
            }
            fz_catch(m_context)
            {
            }
            fz_drop_output(m_context, output);
            output = nullptr;
            file = nullptr;
        }
        if (file)
            ::fclose(file);
        fail(error, fz_caught_message(m_context));
    }

    return saved;
}

} // namespace Mu::Worker::Engine
