// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/render_tracker.hpp"

namespace Mu::Generator {

bool RenderTracker::isLikelyZoomIn(const Okular::PixmapRequest* request)
{
    const Request current { request->observer(), request->pageNumber(), request->width(), request->height() };
    std::lock_guard lock(m_mutex);
    const auto previous = m_previous;
    m_previous = current;
    if (!previous || previous->observer != current.observer || previous->page != current.page)
        return false;

    return current.width >= previous->width && current.height >= previous->height
        && (current.width > previous->width || current.height > previous->height);
}

void RenderTracker::reset()
{
    std::lock_guard lock(m_mutex);
    m_previous.reset();
}

} // namespace Mu::Generator
