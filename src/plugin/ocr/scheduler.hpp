// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <algorithm>

namespace Mu::Plugin::OCR {

enum class ScrollDirection { Forward, Backward };

class Scheduler {
public:
    // Record the latest dominant page. Returning false for an unchanged page
    // lets the controller avoid restarting its debounce timer unnecessarily.
    bool observe(int page, int pageCount)
    {
        if (page == m_page && pageCount == m_pageCount)
            return false;
        m_page = page;
        m_pageCount = pageCount;
        return true;
    }

    QList<int> settle()
    {
        if (m_page < 0 || m_page >= m_pageCount)
            return { };
        if (m_lastPage >= 0 && m_page != m_lastPage)
            m_direction = m_page > m_lastPage ? ScrollDirection::Forward : ScrollDirection::Backward;
        m_lastPage = m_page;

        // Prioritize the focused page, then prefetch one page in the scroll
        // direction and one page behind it.
        QList<int> pages { m_page };
        const int adjacent = m_direction == ScrollDirection::Forward ? m_page + 1 : m_page - 1;
        if (adjacent >= 0 && adjacent < m_pageCount)
            pages.append(adjacent);
        const int opposite = m_page * 2 - adjacent;
        if (opposite >= 0 && opposite < m_pageCount && opposite != adjacent)
            pages.append(opposite);
        return pages;
    }

    // OCR is expensive, so only the current focus page is allowed to keep an
    // active worker job when focus changes.
    bool shouldCancelRunning(int page) const { return m_page >= 0 && page >= 0 && page != m_page; }

    void reset() { *this = { }; }

private:
    int m_page = -1;
    int m_pageCount = 0;
    int m_lastPage = -1;
    ScrollDirection m_direction = ScrollDirection::Forward;
};

struct VisiblePage {
    int page = -1;
    double visibleArea = 0;
};

inline int dominantPage(const QList<VisiblePage>& pages, int previousPage = -1)
{
    // Visible regions can contain multiple entries for one page; aggregate
    // them before choosing the focus page so the result follows actual area.
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

} // namespace Mu::Plugin::OCR
