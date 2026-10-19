// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/types.hpp"
#include <array>
#include <cstdint>

namespace Mu::Model::ThemeColor {

inline constexpr std::uint32_t DefaultBackground = 0xFFFFFF;
inline constexpr std::uint32_t TwilightBackground = 0xD0CAB8;
inline constexpr std::uint32_t TwilightForeground = 0x000000;
inline constexpr std::uint32_t DuskBackground = 0x303134;
inline constexpr std::uint32_t DarkBackground = 0x202124;
inline constexpr std::uint32_t DarkForeground = 0xDADCE0;

constexpr std::uint32_t getBackground(ReadingTheme theme) noexcept
{
    switch (theme) {
    case ReadingTheme::Twilight:
        return TwilightBackground;
    case ReadingTheme::Dusk:
        return DuskBackground;
    case ReadingTheme::Dark:
        return DarkBackground;
    default:
        return DefaultBackground;
    }
}

constexpr std::array<int, 3> splitRgb(std::uint32_t color) noexcept
{
    return { int((color >> 16) & 0xFF), int((color >> 8) & 0xFF), int(color & 0xFF) };
}

} // namespace Mu::Model::ThemeColor
