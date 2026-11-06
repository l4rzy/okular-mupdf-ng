// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/xps/document.hpp"
#include "sys/sys.hpp"

#include <numeric>

namespace Mu::Worker::Engine {

bool XpsDocument::writeExport(Sys::FileDescriptor outputFd,
                              const Model::ExportOptions& options,
                              std::string* error,
                              const ExportProgressCallback&)
{
    std::vector<int> targetPages = options.pages;
    if (targetPages.empty()) {
        targetPages.resize(static_cast<std::size_t>(m_pageCount));
        std::iota(targetPages.begin(), targetPages.end(), 0);
    }

    FILE* volatile file = ::fdopen(outputFd.get(), "wb");
    if (!file)
        return fail(error, "could not adopt output FD");
    (void)outputFd.release();
    fz_document_writer* volatile writer = nullptr;
    fz_page* volatile page = nullptr;
    volatile bool success = false;
    std::string pageError;
    fz_try(m_context)
    {
        // The PDF writer consumes its output, including construction failures.
        writer =
            fz_new_pdf_writer_with_output(m_context, takeOutputFile(m_context, file), Constant::EpubPdfWriterOptions);
        for (const int pageNumber : targetPages) {
            fz_rect bounds { };
            page = loadPageWithBounds(pageNumber, &bounds, &pageError);
            if (!page)
                fz_throw(m_context, FZ_ERROR_FORMAT, "%s", pageError.c_str());
            const fz_rect mediaBox { 0, 0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0 };
            // The writer owns the page device from begin_page through end_page.
            fz_device* device = fz_begin_page(m_context, writer, mediaBox);
            fz_run_page(m_context, page, device, fz_translate(-bounds.x0, -bounds.y0), nullptr);
            fz_end_page(m_context, writer);
            fz_drop_page(m_context, page);
            page = nullptr;
        }
        fz_close_document_writer(m_context, writer);
        success = true;
    }
    fz_always(m_context)
    {
        fz_drop_page(m_context, page);
        fz_drop_document_writer(m_context, writer);
        if (file)
            ::fclose(file);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
    }
    return success;
}

} // namespace Mu::Worker::Engine
