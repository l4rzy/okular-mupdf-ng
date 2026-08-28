// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Construction, Destruction & Context Lifetime
// =============================================================================

PdfDocument::PdfDocument(std::size_t storeSize)
    : m_context(fz_new_context(nullptr, nullptr, storeSize))
{
    if (m_context)
        fz_register_document_handlers(m_context);
}

PdfDocument::~PdfDocument()
{
    close();
    if (m_context)
        fz_drop_context(m_context);
}

// =============================================================================
// Document Opening, Unlocking & Closing
// =============================================================================

// Adopts fd on every path; close() unwinds it on failure. The stream keeps the
// FILE* open via fz_open_file_ptr_no_close, so mupdf does not own it and
// PdfDocument::close() is the sole owner. OCR receives separate descriptors from
// the plugin and never reuses this stream.
bool PdfDocument::openFd(int fd, std::string displayName, std::string* error)
{
    close();
    if (!m_context || fd < 0)
        return fail(error, "input FD is invalid");

    // Convert raw POSIX file descriptor to buffered FILE* stream
    m_input = ::fdopen(fd, "rb");
    if (!m_input) {
        ::close(fd);
        return fail(error, "could not adopt input FD");
    }

    // Query source file size for diagnostic reporting
    struct stat inputStat { };
    if (::fstat(::fileno(m_input), &inputStat) == 0)
        m_sourceSize = static_cast<std::int64_t>(inputStat.st_size);

    // Open Fitz stream and parse PDF document trailer/xref table
    fz_try(m_context)
    {
        m_stream = fz_open_file_ptr_no_close(m_context, m_input);
        m_document = fz_open_document_with_stream(m_context, displayName.c_str(), m_stream);
        m_locked = fz_needs_password(m_context, m_document);
        if (!m_locked) {
            const int declaredCount = fz_count_pages(m_context, m_document);
            m_pageCount = declaredCount;
            if (declaredCount == 0)
                fz_throw(m_context, FZ_ERROR_GENERIC, "document has no loadable pages");
            updateAcroFormPresence();
        }
    }
    fz_catch(m_context)
    {
        close();
        return fail(error, fz_caught_message(m_context));
    }

    m_displayName = std::move(displayName);
    m_resolvedLinkCacheEnabled = true;
    return true;
}

bool PdfDocument::unlock(const std::string& password, std::string* error)
{
    if (!m_document || !m_locked)
        return fail(error, "document is not password locked");

    std::string failureMessage;
    fz_try(m_context)
    {
        // Authenticate with user or owner password
        if (!fz_authenticate_password(m_context, m_document, password.c_str()))
            fz_throw(m_context, FZ_ERROR_ARGUMENT, "incorrect password");

        const int declaredCount = fz_count_pages(m_context, m_document);
        m_pageCount = declaredCount;
        if (declaredCount == 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "document has no loadable pages");

        updateAcroFormPresence();
        m_locked = false;
    }
    fz_catch(m_context)
    {
        failureMessage = fz_caught_message(m_context);
    }

    if (!failureMessage.empty()) {
        close();
        return fail(error, failureMessage.c_str());
    }

    return true;
}

void PdfDocument::close() noexcept
{
    // fz_open_file_ptr_no_close leaves FILE* ownership here. Drop MuPDF objects
    // first, then close the stream and its underlying descriptor exactly once.
    if (m_document && m_context) {
        fz_try(m_context)
        {
            fz_drop_document(m_context, m_document);
        }
        fz_catch(m_context)
        {
        }
    }
    m_document = nullptr;

    if (m_stream && m_context) {
        fz_try(m_context)
        {
            fz_drop_stream(m_context, m_stream);
        }
        fz_catch(m_context)
        {
        }
    }
    m_stream = nullptr;

    if (m_input)
        ::fclose(m_input);
    m_input = nullptr;
    m_sourceSize = 0;

    m_pageCount = 0;
    m_locked = false;
    m_hasAcroForm = false;
    m_displayName.clear();
    m_resolvedLinks.clear();
    m_resolvedLinkKeyBytes = 0;
    m_resolvedLinkCacheEnabled = false;

    // Purge cached textures/fonts and return unused heap pages to OS
    trimProcessMemory(m_context);
}

void PdfDocument::updateAcroFormPresence()
{
    // Cache only the catalog-level presence check; page extraction still walks
    // each page's widgets because field values and widgets may be inherited.
    pdf_document* pdf = pdf_specifics(m_context, m_document);
    m_hasAcroForm =
        pdf && pdf_dict_getl(m_context, pdf_trailer(m_context, pdf), PDF_NAME(Root), PDF_NAME(AcroForm), nullptr);
}

void PdfDocument::discardResolvedLinkCache() noexcept
{
    // This cache is scoped to one incremental link aggregation and must not keep
    // resolved destinations or URI-key storage alive after that operation ends.
    m_resolvedLinks.clear();
    m_resolvedLinkKeyBytes = 0;
    m_resolvedLinkCacheEnabled = false;
}

bool PdfDocument::isOpen() const noexcept
{
    return m_document != nullptr;
}

bool PdfDocument::isLocked() const noexcept
{
    return m_locked;
}

int PdfDocument::pageCount() const noexcept
{
    return m_pageCount;
}

DocumentSettings PdfDocument::settings() const noexcept
{
    return m_settings;
}

void PdfDocument::setSettings(const DocumentSettings& settings) noexcept
{
    m_settings = settings;
    applyFitzSettings(m_context, m_settings);
}

// =============================================================================
// Internal Page Verification & Loading Helpers
// =============================================================================

fz_page* PdfDocument::loadPage(int page, std::string* error) const
{
    if (!m_document || m_locked || page < 0 || page >= m_pageCount) {
        fail(error, "page is unavailable");
        return nullptr;
    }

    fz_page* result = nullptr;
    fz_var(result);
    fz_try(m_context)
    {
        result = fz_load_page(m_context, m_document, page);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
    }

    return result;
}

} // namespace Mu::Worker::Engine
