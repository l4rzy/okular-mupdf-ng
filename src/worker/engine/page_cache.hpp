// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_PAGE_CACHE_HPP
#define MU_WORKER_ENGINE_PAGE_CACHE_HPP

#include <array>
#include <cstddef>

extern "C" {
#include <mupdf/fitz.h>
}

#include "engine/constants.hpp"
#include "shared/logging.hpp"

namespace Mu::Worker::Engine {

/**
 * Bounded most-recently-used cache of parsed MuPDF page handles.
 *
 * Reusing a handle skips MuPDF's page, annotation, link, and resource parsing
 * on repeated renders of the same page (tiled rendering, zoom, pan). The cache
 * owns one reference per slot; callers receive their own kept reference from
 * acquire(). All mutating accessors are const so they can be used from the
 * engines' const loadPage() paths.
 *
 * The engine must call clear() before dropping the fz_context, as it already
 * does in close().
 */
class PageCache {
public:
    PageCache() = default;
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    PageCache(PageCache&&) noexcept = delete;
    PageCache& operator=(PageCache&&) noexcept = delete;
    ~PageCache() = default;

    /// Returns a kept page handle on a hit, promoting the entry to MRU, or
    /// nullptr when the page is not cached.
    [[nodiscard]] fz_page* acquire(fz_context* context, int page) const noexcept
    {
        for (std::size_t index = 0; index < m_entries.size(); ++index) {
            if (m_entries[index].page && m_entries[index].index == page) {
                const Entry hit = m_entries[index];
                for (std::size_t move = index; move > 0; --move)
                    m_entries[move] = m_entries[move - 1];
                m_entries[0] = hit;
                MU_LOG(
                    debug, "Mu::Worker::Engine::PageCache", std::string("Cache hit page: ") + std::to_string(page + 1));
                return fz_keep_page(context, hit.page);
            }
        }
        return nullptr;
    }

    /// Caches a kept reference to loadedPage at MRU, evicting the LRU slot.
    void store(fz_context* context, int page, fz_page* loadedPage) const noexcept
    {
        if (!loadedPage)
            return;

        dropEntry(context, m_entries.back());
        for (std::size_t index = m_entries.size() - 1; index > 0; --index)
            m_entries[index] = m_entries[index - 1];
        m_entries[0] = Entry { page, fz_keep_page(context, loadedPage) };
    }

    /// Drops every cached page handle and resets the cache.
    void clear(fz_context* context) const noexcept
    {
        for (Entry& entry : m_entries)
            dropEntry(context, entry);
    }

    [[nodiscard]] bool contains(int page) const noexcept
    {
        for (const Entry& entry : m_entries) {
            if (entry.page && entry.index == page)
                return true;
        }
        return false;
    }

private:
    struct Entry {
        int index = -1;
        fz_page* page = nullptr;
    };

    void dropEntry(fz_context* context, Entry& entry) const noexcept
    {
        MU_LOG(debug,
               "Mu::Worker::Engine::PageCache",
               std::string("Cache evicted page: ") + std::to_string(entry.index + 1));
        if (!entry.page)
            return;
        if (context) {
            fz_try(context)
            {
                fz_drop_page(context, entry.page);
            }
            fz_catch(context)
            {
            }
        }
        entry = { };
    }

    mutable std::array<Entry, Constant::PageCacheSize> m_entries { };
};

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_PAGE_CACHE_HPP
