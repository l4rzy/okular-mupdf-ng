// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_RENDER_TRACKER_HPP
#define MU_GENERATOR_RENDER_TRACKER_HPP

#include <okular/core/generator.h>

#include <mutex>
#include <optional>

namespace Mu::Generator {

/// Tracks Okular render requests to identify likely zoom-in transitions.
class RenderTracker final {
public:
    // Records the request and returns whether it is a likely zoom-in render.
    bool isLikelyZoomIn(const Okular::PixmapRequest* request);
    // Clears request history when the active document changes.
    void reset();

private:
    struct Request {
        const Okular::DocumentObserver* observer = nullptr;
        int page = -1;
        int width = 0;
        int height = 0;
    };

    std::mutex m_mutex;
    std::optional<Request> m_previous;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_RENDER_TRACKER_HPP
