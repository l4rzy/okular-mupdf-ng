// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/model/export.hpp"

#include "shared/protocol/limits.hpp"
#include <algorithm>
#include <utility>

namespace Mu::Model {

bool isValidExportOptions(const ExportOptions& options, std::string_view* reason) noexcept
{
    const auto fail = [&](std::string_view text) {
        if (reason)
            *reason = text;
        return false;
    };
    constexpr auto knownFlags = std::to_underlying(ExportFlag::PageSelection)
        | std::to_underlying(ExportFlag::IncludeAnnotations) | std::to_underlying(ExportFlag::FlattenAnnotations)
        | std::to_underlying(ExportFlag::IncludeReferences);
    const auto isValidIdentifier = [](std::string_view identifier) {
        constexpr std::size_t MaxIdentifierBytes = 128;
        return !identifier.empty() && identifier.size() <= MaxIdentifierBytes
            && identifier.find('\0') == std::string_view::npos;
    };
    if (!isValidIdentifier(options.formatId))
        return fail("invalid export format identifier");
    if (options.profileId == ExportProfileId::Invalid || options.profileId > ExportProfileId::Converted)
        return fail("invalid export profile");
    if (options.flags & ~knownFlags)
        return fail("unknown export flags");
    if (options.pages.size() > ::Mu::Limit::MaxExportPages)
        return fail("too many export pages");
    if (std::ranges::any_of(options.pages, [](auto page) { return page < 0; }))
        return fail("negative export page");
    return true;
}

const ExportProfile* findExportProfile(const std::vector<OutputFormatInfo>& formats,
                                       const ExportOptions& options,
                                       bool asynchronous,
                                       std::int32_t pageCount,
                                       std::string* error)
{
    const auto fail = [&](std::string_view text) -> const ExportProfile* {
        if (error)
            *error = text;
        return nullptr;
    };
    std::string_view reason;
    if (!isValidExportOptions(options, &reason))
        return fail(reason);
    if (pageCount < 0 || options.pages.size() > static_cast<std::size_t>(pageCount)
        || std::ranges::any_of(options.pages, [pageCount](auto page) { return page >= pageCount; }))
        return fail("export page selection is out of range");
    const auto format = std::ranges::find(formats, options.formatId, &OutputFormatInfo::id);
    if (format == formats.end())
        return fail("unsupported export format");
    const auto profile = std::ranges::find(format->profiles, options.profileId, &ExportProfile::id);
    if (profile == format->profiles.end())
        return fail("unsupported export profile");
    const bool supportsExecution = asynchronous ? profile->asynchronous : profile->synchronous;
    if (!supportsExecution)
        return fail("unsupported export execution mode");
    const auto requiredFlags =
        options.flags | (options.pages.empty() ? 0U : std::to_underlying(ExportFlag::PageSelection));
    if (requiredFlags & ~profile->supportedFlags)
        return fail("unsupported export options");
    return &*profile;
}

} // namespace Mu::Model
