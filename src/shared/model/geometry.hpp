// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_MODEL_GEOMETRY_HPP
#define MU_SHARED_MODEL_GEOMETRY_HPP

#include <algorithm>

#include "shared/model/types.hpp"

namespace Mu::Model {

/// The model stores annotation geometry normalized to its page: the origin is
/// the top-left corner, X grows right, and Y grows downward, matching the
/// display frame used by the renderer and the UI. PDF and XFDF instead use
/// user-space points with the origin at the bottom-left and Y growing upward.
///
/// These two conversions are the single owner of the normalized <-> user-space
/// contract. Import and export must both call them so the vertical axis flip
/// cannot drift between the two directions.

/// Converts a normalized top-left page point to PDF user-space points
/// (bottom-left origin, Y up) for a page of the given point extents.
[[nodiscard]] inline Point
normalizedToUserSpace(const Point& value, double pageWidthPoints, double pageHeightPoints) noexcept
{
    return { value.x * pageWidthPoints, (1.0 - value.y) * pageHeightPoints };
}

/// Converts a PDF user-space point (bottom-left origin, Y up) to a normalized
/// top-left page point clamped to [0, 1]. A non-positive page extent yields a
/// zero coordinate rather than an infinity.
[[nodiscard]] inline Point
userSpaceToNormalized(const Point& value, double pageWidthPoints, double pageHeightPoints) noexcept
{
    const double x = pageWidthPoints > 0 ? value.x / pageWidthPoints : 0.0;
    const double y = pageHeightPoints > 0 ? 1.0 - value.y / pageHeightPoints : 0.0;
    const auto clamp01 = [](double component) noexcept {
        return std::clamp(component, 0.0, 1.0);
    };
    return { clamp01(x), clamp01(y) };
}

} // namespace Mu::Model

#endif // MU_SHARED_MODEL_GEOMETRY_HPP
