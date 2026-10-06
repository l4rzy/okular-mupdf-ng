// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_WORKER_PDF_DESTINATION_HPP
#define MU_WORKER_PDF_DESTINATION_HPP

#include "engine/constants.hpp"

#include <algorithm>
#include <cmath>

namespace Mu::Worker::Engine {

/// Leaves space above a navigation target, without scrolling beyond the page top.
inline double normalizeDestinationY(double y, double heightPoints)
{
    return std::isfinite(heightPoints) && heightPoints > Constant::DestinationTopMarginPoints
        ? std::max(0.0, y - Constant::DestinationTopMarginPoints / heightPoints)
        : y;
}

} // namespace Mu::Worker::Engine
#endif
