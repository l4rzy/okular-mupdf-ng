// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/constants.hpp"
#include "engine/epub/document.hpp"

#include <cstdio>
#include <unistd.h>

extern "C" {
#include <mupdf/fitz.h>
}

namespace Mu::Worker::Engine {

// =============================================================================
// EPUB-to-PDF Conversion & Export
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

} // namespace Mu::Worker::Engine
