// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CACHING_CACHE_ROOT_HPP
#define MU_PLUGIN_CACHING_CACHE_ROOT_HPP

#include <filesystem>

namespace Mu::Plugin::Caching {

[[nodiscard]] std::filesystem::path cacheRoot();

} // namespace Mu::Plugin::Caching

#endif // MU_PLUGIN_CACHING_CACHE_ROOT_HPP
