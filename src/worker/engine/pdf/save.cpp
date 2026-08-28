// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <array>
#include <cstdio>
#include <unistd.h>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

// =============================================================================
// Helper Functions for Safe File Writing & Cleanup
// =============================================================================

namespace {

/// Helper to replicate file contents from source to destination stream before incremental append.
bool copyFileContents(FILE* src, FILE* dst)
{
    if (!src || !dst)
        return false;
    if (::fseek(src, 0, SEEK_SET) != 0)
        return false;

    std::array<unsigned char, Constant::FileCopyChunkBytes> buffer;
    for (;;) {
        const std::size_t readCount = ::fread(buffer.data(), 1, buffer.size(), src);
        if (readCount > 0) {
            if (::fwrite(buffer.data(), 1, readCount, dst) != readCount)
                return false;
        }
        if (readCount < buffer.size()) {
            if (::ferror(src))
                return false;
            break;
        }
    }
    return ::fflush(dst) == 0;
}

/// Helper to safely close and release Fitz output stream without leaking file descriptors.
void closeAndDropOutput(fz_context* context, fz_output*& output, FILE*& file) noexcept
{
    // This helper is used only after a failed write. Closing output is best
    // effort; the original MuPDF/write error remains the caller's diagnostic.
    if (output) {
        fz_try(context)
        {
            fz_close_output(context, output);
        }
        fz_catch(context)
        {
        }
        fz_drop_output(context, output);
        output = nullptr;
        file = nullptr;
    }
    if (file) {
        ::fclose(file);
        file = nullptr;
    }
}

} // namespace

// =============================================================================
// Full Document Saving
// =============================================================================

// Consumes fd on every path. fz_output owns the FILE* after construction,
// so it is deliberately never fclose'd after fz_close_output().
bool PdfDocument::saveFd(int fd, std::string* error)
{
    if (fd < 0)
        return fail(error, "output FD is invalid");

    pdf_document* pdf = pdf_specifics(m_context, m_document);
    if (!m_document || m_locked || !pdf) {
        ::close(fd);
        return fail(error, "document cannot be saved");
    }

    FILE* file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }

    fz_output* output = nullptr;
    bool saved = false;
    fz_var(output);
    fz_var(saved);
    fz_var(file);

    fz_try(m_context)
    {
        pdf_write_options options = pdf_default_write_options;

        // If possible, attempt incremental save to preserve digital signatures and structure
        if (m_input && pdf_can_be_saved_incrementally(m_context, pdf)) {
            if (copyFileContents(m_input, file)) {
                options.do_incremental = 1;
            } else {
                // Fall back to full rewrite if copying source stream fails
                ::fflush(file);
                if (::ftruncate(::fileno(file), 0) == 0)
                    (void)::fseek(file, 0, SEEK_SET);
            }
        }

        output = fz_new_output_with_file_ptr(m_context, file);
        // MuPDF output owns FILE* from this point and closes it during output
        // close. Null the local owner so failure cleanup cannot double-close it.
        pdf_update_open_pages(m_context, pdf);
        pdf_write_document(m_context, pdf, output, &options);

        fz_flush_output(m_context, output);
        if (::fflush(file) != 0 || ::fsync(::fileno(file)) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "failed to flush save output");

        fz_close_output(m_context, output);
        fz_drop_output(m_context, output);
        output = nullptr;
        file = nullptr;
        saved = true;
    }
    fz_catch(m_context)
    {
        closeAndDropOutput(m_context, output, file);
        fail(error, fz_caught_message(m_context));
    }

    return saved;
}

// =============================================================================
// Page Subset PDF Export
// =============================================================================

bool PdfDocument::savePdfFd(int fd, const std::vector<int>& pages, std::string* error)
{
    // The output descriptor is consumed on every path; page selection is
    // copied before entering the MuPDF writer boundary.
    if (fd < 0)
        return fail(error, "output FD is invalid");

    pdf_document* srcDoc = pdf_specifics(m_context, m_document);
    if (!m_document || m_locked || !srcDoc) {
        ::close(fd);
        return fail(error, "document cannot be exported to PDF");
    }
    for (const int page : pages) {
        if (page < 0 || page >= m_pageCount) {
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
    pdf_document* dstDoc = nullptr;
    pdf_graft_map* map = nullptr;
    bool saved = false;
    fz_var(output);
    fz_var(dstDoc);
    fz_var(map);
    fz_var(saved);
    fz_var(file);

    std::vector<int> targetPages = pages;
    if (targetPages.empty()) {
        targetPages.reserve(static_cast<std::size_t>(m_pageCount));
        for (int p = 0; p < m_pageCount; ++p)
            targetPages.push_back(p);
    }

    fz_try(m_context)
    {
        output = fz_new_output_with_file_ptr(m_context, file);
        dstDoc = pdf_create_document(m_context);
        map = pdf_new_graft_map(m_context, dstDoc);

        // Graft specified pages and their resource dependencies into the destination document
        int dstIndex = 0;
        for (const int p : targetPages) {
            if (p < 0 || p >= m_pageCount)
                continue;
            pdf_graft_mapped_page(m_context, map, dstIndex++, srcDoc, p);
        }

        pdf_write_options options = pdf_default_write_options;
        pdf_update_open_pages(m_context, dstDoc);
        pdf_write_document(m_context, dstDoc, output, &options);

        fz_flush_output(m_context, output);
        if (::fflush(file) != 0 || ::fsync(::fileno(file)) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "failed to flush save output");

        fz_close_output(m_context, output);
        fz_drop_output(m_context, output);
        output = nullptr;
        file = nullptr;
        saved = true;
    }
    fz_always(m_context)
    {
        if (map)
            pdf_drop_graft_map(m_context, map);
        if (dstDoc)
            pdf_drop_document(m_context, dstDoc);
    }
    fz_catch(m_context)
    {
        closeAndDropOutput(m_context, output, file);
        fail(error, fz_caught_message(m_context));
    }

    return saved;
}

} // namespace Mu::Worker::Engine
