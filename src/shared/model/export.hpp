// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_MODEL_EXPORT_HPP
#define MU_SHARED_MODEL_EXPORT_HPP
#include "shared/model/types.hpp"
#include <string_view>

namespace Mu::Model {

bool isValidExportOptions(const ExportOptions& options, std::string_view* reason = nullptr) noexcept;

// Returns a borrowed profile only if this exact operation is supported. Empty
// selection means all pages; no implicit profile/format/quality fallback.
const ExportProfile* findExportProfile(const std::vector<OutputFormatInfo>& formats,
                                       const ExportOptions& options,
                                       bool asynchronous,
                                       std::int32_t pageCount,
                                       std::string* error = nullptr);

} // namespace Mu::Model
#endif
