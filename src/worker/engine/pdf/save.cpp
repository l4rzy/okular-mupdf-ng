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
void closeAndDropOutput(fz_context* context, fz_output* volatile& output, FILE* volatile& file) noexcept
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

    // Drop the cached pages before pdf_update_open_pages()/serialization so the
    // write sees the same open-page set it would without the render cache.
    clearPageCache();

    FILE* volatile file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }

    fz_output* volatile output = nullptr;
    volatile bool saved = false;

    fz_try(m_context)
    {
        pdf_write_options options = pdf_default_write_options;

        // If possible, attempt incremental save to preserve digital signatures and structure
        if (m_input && pdf_can_be_saved_incrementally(m_context, pdf)) {
            if (copyFileContents(m_input, file)) {
                options.do_incremental = 1;
            } else {
                // Fail closed: the staged prefix may be partial, so a full
                // rewrite over it could publish a corrupt file. The plugin
                // discards its temp file and the destination stays untouched.
                fz_throw(m_context, FZ_ERROR_GENERIC, "could not stage source for incremental save");
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

// Flatten a grafted catalog rather than serializing or baking the live document.
// The shared graft map preserves cyclic references, forms, outlines and resources,
// while reading the current objects includes edits that have not been saved yet.
bool PdfDocument::flattenPdfFd(int fd, const std::vector<int>& pages, std::string* error)
{
    if (fd < 0)
        return fail(error, "output FD is invalid");
    if (!m_document || m_locked) {
        ::close(fd);
        return fail(error, "document cannot be flattened");
    }
    if (pages.size() > static_cast<std::size_t>(m_pageCount)) {
        ::close(fd);
        return fail(error, "export page selection is too large");
    }
    for (const int page : pages) {
        if (page < 0 || page >= m_pageCount) {
            ::close(fd);
            return fail(error, "export page is out of range");
        }
    }

    FILE* volatile file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }
    fz_output* volatile output = nullptr;
    pdf_document* volatile copy = nullptr;
    pdf_graft_map* volatile map = nullptr;
    volatile bool saved = false;
    fz_try(m_context)
    {
        pdf_document* source = pdf_specifics(m_context, m_document);
        copy = pdf_create_document(m_context);
        copy->version = source->version;
        map = pdf_new_graft_map(m_context, copy);
        pdf_obj* sourceTrailer = pdf_trailer(m_context, source);
        if (pdf_dict_getp(m_context, sourceTrailer, "Root/AcroForm/XFA"))
            fz_throw(m_context, FZ_ERROR_ARGUMENT, "XFA forms cannot be flattened");
        pdf_obj* copyTrailer = pdf_trailer(m_context, copy);
        pdf_dict_put_drop(
            m_context,
            copyTrailer,
            PDF_NAME(Root),
            pdf_graft_mapped_object(m_context, map, pdf_dict_get(m_context, sourceTrailer, PDF_NAME(Root))));
        pdf_obj* info = pdf_dict_get(m_context, sourceTrailer, PDF_NAME(Info));
        if (info)
            pdf_dict_put_drop(m_context, copyTrailer, PDF_NAME(Info), pdf_graft_mapped_object(m_context, map, info));
        if (!pages.empty())
            pdf_rearrange_pages(
                m_context, copy, static_cast<int>(pages.size()), pages.data(), PDF_CLEAN_STRUCTURE_KEEP);
        pdf_bake_document(m_context, copy, 1, 1);

        // MuPDF can abandon a failed bake without rethrowing. Never publish an
        // interactive document as a successful flattened export. Links remain.
        if (pdf_dict_getp(m_context, pdf_trailer(m_context, copy), "Root/AcroForm"))
            fz_throw(m_context, FZ_ERROR_GENERIC, "form flattening did not complete");
        for (int page = 0; page < pdf_count_pages(m_context, copy); ++page) {
            pdf_obj* annots = pdf_dict_get(m_context, pdf_lookup_page_obj(m_context, copy, page), PDF_NAME(Annots));
            for (int index = 0; index < pdf_array_len(m_context, annots); ++index) {
                pdf_obj* annot = pdf_array_get(m_context, annots, index);
                if (!pdf_name_eq(m_context, pdf_dict_get(m_context, annot, PDF_NAME(Subtype)), PDF_NAME(Link)))
                    fz_throw(m_context, FZ_ERROR_GENERIC, "annotation flattening did not complete");
            }
        }

        pdf_write_options options = pdf_default_write_options;
        options.do_garbage = 1;
        options.do_encrypt = PDF_ENCRYPT_NONE;
        output = fz_new_output_with_file_ptr(m_context, file);
        pdf_write_document(m_context, copy, output, &options);
        fz_flush_output(m_context, output);
        if (::fflush(file) != 0 || ::fsync(::fileno(file)) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "failed to flush flattened PDF output");
        fz_close_output(m_context, output);
        fz_drop_output(m_context, output);
        output = nullptr;
        file = nullptr;
        saved = true;
    }
    fz_always(m_context)
    {
        pdf_drop_graft_map(m_context, map);
        pdf_drop_document(m_context, copy);
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
    // A selection longer than the page count must contain duplicates, which
    // only multiply graft work and output size. Reject it before copying.
    if (pages.size() > static_cast<std::size_t>(m_pageCount)) {
        ::close(fd);
        return fail(error, "export page selection is too large");
    }

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

    FILE* volatile file = ::fdopen(fd, "wb");
    if (!file) {
        ::close(fd);
        return fail(error, "could not adopt output FD");
    }

    fz_output* volatile output = nullptr;
    pdf_document* volatile dstDoc = nullptr;
    pdf_graft_map* volatile map = nullptr;
    volatile bool saved = false;

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
