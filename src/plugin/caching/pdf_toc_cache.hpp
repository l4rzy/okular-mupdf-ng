// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_PLUGIN_PDF_TOC_CACHE_HPP
#define MU_PLUGIN_PDF_TOC_CACHE_HPP

#include "shared/model/types.hpp"
#include <QString>
#include <optional>
#include <vector>

namespace Mu::Plugin::Caching::PDF {

/// Hashes the open source with pread, leaving MuPDF's shared FD offset intact.
/// Algorithm revisions change the namespace and invalidate old results.
QString tocCachePath(int sourceFd);
std::optional<std::vector<Model::OutlineNode>> loadToc(const QString& cachePath, int pageCount);
bool saveToc(const QString& cachePath, int pageCount, const std::vector<Model::OutlineNode>& nodes);

} // namespace Mu::Plugin::Caching::PDF
#endif
