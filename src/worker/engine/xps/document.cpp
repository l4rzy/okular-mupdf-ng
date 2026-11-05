// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/xps/document.hpp"
#include "runtime/memory_pressure.hpp"

#include <cmath>
#include <unistd.h>

namespace Mu::Worker::Engine {

XpsDocument::XpsDocument(std::size_t storeSize)
    : m_context(createContext(storeSize))
{
    if (!m_context)
        return;
    fz_try(m_context)
    {
        fz_register_document_handlers(m_context);
    }
    fz_catch(m_context)
    {
        fz_drop_context(m_context);
        m_context = nullptr;
    }
}

XpsDocument::~XpsDocument()
{
    close();
    fz_drop_context(m_context);
}

bool XpsDocument::openFd(int fd, std::string, std::string* error)
{
    close();
    if (fd < 0)
        return fail(error, "input FD is invalid");
    if (!m_context) {
        ::close(fd);
        return fail(error, "MuPDF context is unavailable");
    }
    m_input = ::fdopen(fd, "rb");
    if (!m_input) {
        ::close(fd);
        return fail(error, "could not adopt input FD");
    }
    fz_try(m_context)
    {
        m_stream = fz_open_file_ptr_no_close(m_context, m_input);
        // A filename hint still permits content sniffing to select another
        // parser. Open through the XPS handler explicitly instead.
        const auto* handler = fz_recognize_document(m_context, "application/vnd.ms-xpsdocument");
        if (!handler)
            fz_throw(m_context, FZ_ERROR_UNSUPPORTED, "XPS support is unavailable");
        m_document = handler->open(m_context, handler, m_stream, nullptr, nullptr, nullptr);
        m_pageCount = fz_count_pages(m_context, m_document);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        close();
        return false;
    }
    return true;
}

bool XpsDocument::unlock(const std::string&, std::string* error)
{
    return fail(error, "XPS document is not password locked");
}

void XpsDocument::close() noexcept
{
    m_pageCache.clear(m_context);
    if (m_context) {
        fz_try(m_context)
        {
            fz_drop_document(m_context, m_document);
            fz_drop_stream(m_context, m_stream);
        }
        fz_catch(m_context)
        {
        }
    }
    m_document = nullptr;
    m_stream = nullptr;
    if (m_input)
        ::fclose(m_input);
    m_input = nullptr;
    m_pageCount = 0;
    m_fonts.reset();
    m_hash.reset();
    discardResolvedLinkCache();
    trimProcessMemory(m_context);
}

void XpsDocument::trimMemoryForIdle() noexcept
{
    trimIdleStore(m_context,
                  Runtime::MemoryPressure::idleTrimThresholdsForLevel(m_settings.idleTrimAggressiveness).storePercent);
}

void XpsDocument::discardResolvedLinkCache() noexcept
{
    m_resolvedLinks.clear();
    m_resolvedLinkKeyBytes = 0;
}

void XpsDocument::setSettings(const DocumentSettings& settings) noexcept
{
    m_settings = settings;
    m_settings.idleTrimAggressiveness = Runtime::MemoryPressure::normalizeIdleTrim(m_settings.idleTrimAggressiveness);
    applyFitzSettings(m_context, m_settings);
}

void XpsDocument::setPageCacheSuspended(bool suspended) noexcept
{
    m_pageCache.setSuspended(suspended);
}

fz_page* XpsDocument::loadPage(int page, std::string* error) const
{
    if (!m_document || page < 0 || page >= m_pageCount) {
        fail(error, "page is unavailable");
        return nullptr;
    }
    if (fz_page* cached = m_pageCache.acquire(m_context, page))
        return cached;
    fz_page* volatile result = nullptr;
    fz_try(m_context)
    {
        result = fz_load_page(m_context, m_document, page);
        m_pageCache.store(m_context, page, result);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
    }
    return result;
}

fz_page* XpsDocument::loadPageWithBounds(int page, fz_rect* bounds, std::string* error) const
{
    fz_page* volatile result = loadPage(page, error);
    if (!result)
        return nullptr;
    fz_try(m_context)
    {
        *bounds = fz_bound_page(m_context, result);
        const float width = bounds->x1 - bounds->x0;
        const float height = bounds->y1 - bounds->y0;
        if (!std::isfinite(bounds->x0) || !std::isfinite(bounds->y0) || !std::isfinite(bounds->x1)
            || !std::isfinite(bounds->y1) || !std::isfinite(width) || !std::isfinite(height) || width <= 0
            || height <= 0)
            fz_throw(m_context, FZ_ERROR_FORMAT, "XPS page bounds are invalid");
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        fz_drop_page(m_context, result);
        result = nullptr;
    }
    return result;
}

} // namespace Mu::Worker::Engine
