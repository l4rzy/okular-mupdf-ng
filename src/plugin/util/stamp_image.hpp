// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_STAMP_IMAGE_HPP
#define MU_PLUGIN_UTIL_STAMP_IMAGE_HPP

#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

namespace Mu::Plugin::Util {

// Empty bytes select a native PDF stamp; nullopt means an icon/file could not be loaded.
std::optional<std::vector<std::uint8_t>> prepareStampImage(const QString& symbol);

} // namespace Mu::Plugin::Util

#endif
