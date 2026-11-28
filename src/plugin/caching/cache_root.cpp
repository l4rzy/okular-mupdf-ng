// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/cache_root.hpp"

#include <QFile>
#include <QStandardPaths>

namespace Mu::Plugin::Caching {

std::filesystem::path cacheRoot()
{
    const auto location = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (location.isEmpty())
        return { };
    return std::filesystem::path(QFile::encodeName(location).constData()) / "mupdfng";
}

} // namespace Mu::Plugin::Caching
