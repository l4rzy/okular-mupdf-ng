// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_OCR_POLICY_HPP
#define MU_PLUGIN_OCR_POLICY_HPP

#include <QHash>
#include <QList>

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "plugin/ocr/constants.hpp"

namespace Mu::Plugin::OCR {

enum class ScrollDirection { Forward, Backward };

struct VisiblePage {
    int page = -1;
    double visibleArea = 0;
};

// Picks the page that dominates the viewport, aggregating multiple visible
// regions of the same page. Ties favour @p previousPage, then the lowest page.
inline int dominantPage(const QList<VisiblePage>& pages, int previousPage = -1)
{
    QList<VisiblePage> totals;
    for (const auto& page : pages) {
        if (page.page < 0 || page.visibleArea <= 0)
            continue;
        auto total =
            std::find_if(totals.begin(), totals.end(), [page](const auto& value) { return value.page == page.page; });
        if (total == totals.end())
            totals.append(page);
        else
            total->visibleArea += page.visibleArea;
    }

    int result = -1;
    double largest = 0;
    for (const auto& page : totals) {
        if (page.visibleArea > largest || (page.visibleArea == largest && page.page == previousPage)
            || (page.visibleArea == largest && result != previousPage && (result < 0 || page.page < result))) {
            result = page.page;
            largest = page.visibleArea;
        }
    }
    return result;
}

// Tracks the settled focus page and the scrolling direction that produced it.
// The prefetch window is a cache hint only: worker OCR is limited to focus(),
// because a worker job cannot be interrupted once it starts.
class FocusPolicy {
public:
    // Records the latest dominant page. Returns false when nothing changed so
    // the caller can avoid restarting its debounce timer.
    bool noteFocus(int page, int pageCount)
    {
        if (page == m_focus && pageCount == m_pageCount)
            return false;
        if (m_focus >= 0 && page != m_focus)
            m_direction = page > m_focus ? ScrollDirection::Forward : ScrollDirection::Backward;
        m_focus = page;
        m_pageCount = pageCount;
        return true;
    }

    [[nodiscard]] int focus() const { return m_focus; }

    // Focus first, then one page ahead in the scroll direction and one behind.
    // Returns an empty list when no page dominates.
    [[nodiscard]] QList<int> prefetchWindow() const
    {
        if (m_focus < 0 || m_focus >= m_pageCount)
            return { };
        QList<int> pages { m_focus };
        const int adjacent = m_direction == ScrollDirection::Forward ? m_focus + 1 : m_focus - 1;
        if (adjacent >= 0 && adjacent < m_pageCount)
            pages.append(adjacent);
        const int opposite = m_focus * 2 - adjacent;
        if (opposite >= 0 && opposite < m_pageCount && opposite != adjacent)
            pages.append(opposite);
        return pages;
    }

    void reset() { *this = { }; }

private:
    int m_focus = -1;
    int m_pageCount = 0;
    ScrollDirection m_direction = ScrollDirection::Forward;
};

// Ordered set of pages awaiting cache probing or worker OCR. Ordering is by
// distance to the focus so the page the user is looking at is dispatched first.
// Pages that drift beyond the staleness radius are dropped rather than
// cancelled, since an in-flight worker job cannot be interrupted.
class PageQueue {
public:
    // Rebuilds the queue from the current candidates, retaining relevant queued
    // work behind them, then orders nearest-first and trims to the bounds.
    void refresh(int focus, const QList<int>& candidates, int radius, int maxPages)
    {
        QList<int> merged;
        for (int page : candidates) {
            if (!merged.contains(page))
                merged.append(page);
        }
        for (int page : m_queue) {
            if (!merged.contains(page))
                merged.append(page);
        }

        m_queue.clear();
        if (focus < 0)
            return;
        m_queue = merged;
        dropStale(focus, radius);
        std::stable_sort(m_queue.begin(), m_queue.end(), [focus](int a, int b) {
            return std::abs(a - focus) < std::abs(b - focus);
        });
        while (m_queue.size() > maxPages)
            m_queue.removeLast();
    }

    // Removes and returns the page nearest the focus, dropping stale entries
    // first so a page the user has left is never dispatched.
    std::optional<int> takeNext(int focus, int radius)
    {
        dropStale(focus, radius);
        if (m_queue.isEmpty())
            return std::nullopt;
        return m_queue.takeFirst();
    }

    // Reinserts a page whose dispatch or recognition failed. A page that is
    // already stale is not resurrected.
    void pushFront(int page, int focus, int radius)
    {
        if (page < 0)
            return;
        m_queue.removeAll(page);
        m_queue.prepend(page);
        dropStale(focus, radius);
    }

    [[nodiscard]] bool contains(int page) const { return m_queue.contains(page); }

    [[nodiscard]] bool isEmpty() const { return m_queue.isEmpty(); }

    [[nodiscard]] const QList<int>& pages() const { return m_queue; }

    void clear() { m_queue.clear(); }

private:
    void dropStale(int focus, int radius)
    {
        if (focus < 0) {
            m_queue.clear();
            return;
        }
        for (auto it = m_queue.begin(); it != m_queue.end();) {
            if (std::abs(*it - focus) > radius)
                it = m_queue.erase(it);
            else
                ++it;
        }
    }

    QList<int> m_queue;
};

// Bounds repeated attempts for the same page. An attempt is only counted when
// it fails, so a page is dispatched up to maxAttempts times in total.
class RetryPolicy {
public:
    explicit RetryPolicy(int maxAttempts = Constant::MAX_ATTEMPTS)
        : m_maxAttempts(maxAttempts)
    {
    }

    // Records a failure; returns true when another attempt is permitted.
    bool onFailure(int page)
    {
        const int attempts = m_attempts.value(page, 0) + 1;
        m_attempts.insert(page, attempts);
        return attempts < m_maxAttempts;
    }

    void clear(int page) { m_attempts.remove(page); }

    [[nodiscard]] bool exhausted(int page) const { return m_attempts.value(page, 0) >= m_maxAttempts; }

    void clear() { m_attempts.clear(); }

private:
    int m_maxAttempts;
    QHash<int, int> m_attempts;
};

} // namespace Mu::Plugin::OCR
#endif // MU_PLUGIN_OCR_POLICY_HPP
