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

constexpr bool isCustom(DocumentTheme theme) noexcept
{
    return theme == DocumentTheme::Custom1 || theme == DocumentTheme::Custom2;
}

constexpr const ThemeCustomization&
customization(DocumentTheme theme, const ThemeCustomization& first, const ThemeCustomization& second) noexcept
{
    return theme == DocumentTheme::Custom2 ? second : first;
}

constexpr std::uint32_t getBackground(DocumentTheme theme) noexcept
{
    switch (theme) {
    case DocumentTheme::Twilight:
        return TwilightBackground;
    case DocumentTheme::Dusk:
        return DuskBackground;
    case DocumentTheme::Dark:
        return DarkBackground;
    case DocumentTheme::Custom1:
    case DocumentTheme::Custom2:
        return DarkBackground;
    default:
        return DefaultBackground;
    }
}

constexpr std::uint32_t getBackground(DocumentTheme theme, std::uint32_t customBackground) noexcept
{
    return isCustom(theme) ? customBackground : getBackground(theme);
}

constexpr std::array<int, 3> splitRgb(std::uint32_t color) noexcept
{
    return { int((color >> 16) & 0xFF), int((color >> 8) & 0xFF), int(color & 0xFF) };
}

} // namespace Mu::Model::ThemeColor
