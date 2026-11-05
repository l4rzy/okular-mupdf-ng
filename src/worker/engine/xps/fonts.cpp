// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/xps/document.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string_view>

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Helper Functions for ZIP Archive Font Enumeration
// =============================================================================

namespace {

/// Converts string to lowercase for case-insensitive extension matching.
std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

/// Identifies font file extensions (.ttf, .otf, .woff, .woff2).
std::string fontExtension(std::string_view entry)
{
    const std::size_t dot = entry.rfind('.');
    if (dot == std::string_view::npos)
        return { };

    const std::string suffix = lower(entry.substr(dot));
    if (suffix != ".ttf" && suffix != ".ttc" && suffix != ".otf" && suffix != ".otc" && suffix != ".woff"
        && suffix != ".woff2" && suffix != ".odttf")
        return { };
    return suffix;
}

/// Extracts base font name from archive file path.
std::string fontNameFromEntry(std::string_view entry)
{
    const std::size_t slash = entry.rfind('/');
    const std::size_t dot = entry.rfind('.');
    const std::size_t start = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t end = dot == std::string_view::npos || dot < start ? entry.size() : dot;
    return std::string(entry.substr(start, end - start));
}

/// Maps font file extension to FontType model enum.
FontType fontType(std::string_view extension)
{
    if (extension == ".ttf" || extension == ".ttc" || extension == ".odttf")
        return FontType::TrueType;
    if (extension == ".otf" || extension == ".otc" || extension == ".woff" || extension == ".woff2")
        return FontType::TrueTypeOT;
    return FontType::Unknown;
}

} // namespace

// =============================================================================
// Embedded Font Enumeration
// =============================================================================

std::vector<Font> XpsDocument::fonts(const std::vector<int>&, std::string* error) const
{
    if (m_fonts)
        return *m_fonts;

    if (!m_document) {
        fail(error, "document is unavailable");
        return { };
    }
    std::vector<Font> result;
    ByteBudget budget;

    // Open the XPS ZIP container and snapshot the entry count with PODs
    // only; every std::string/vector operation happens outside the boundary.
    volatile int entryCount = 0;
    fz_archive* volatile archive = nullptr;
    fz_try(m_context)
    {
        archive = fz_open_zip_archive_with_stream(m_context, m_stream);
        entryCount = fz_count_archive_entries(m_context, archive);
    }
    fz_catch(m_context)
    {
        fz_drop_archive(m_context, archive);
        fail(error, fz_caught_message(m_context));
        return { };
    }
    const auto dropArchive = [this](fz_archive* value) {
        fz_drop_archive(m_context, value);
    };
    const std::unique_ptr<fz_archive, decltype(dropArchive)> owner(archive, dropArchive);
    if (entryCount < 0 || static_cast<std::size_t>(entryCount) > Limit::MaxContainerAllocationBytes / sizeof(Font)) {
        fail(error, "resource limit: XPS archive has too many entries");
        return { };
    }

    for (int index = 0; index < entryCount; ++index) {
        const char* volatile entryName = nullptr;
        fz_try(m_context)
        {
            entryName = fz_list_archive_entry(m_context, archive, index);
        }
        fz_catch(m_context)
        {
            fail(error, fz_caught_message(m_context));
            return { };
        }
        if (!entryName)
            continue;

        // Charge names before constructing strings from untrusted ZIP entries.
        const auto bytes = strnlen(entryName, Limit::MaxString + 1);
        if (bytes > Limit::MaxString) {
            fail(error, ExtractionBudgetError);
            return { };
        }
        const std::string_view entry(entryName, bytes);
        const std::string extension = fontExtension(entry);
        if (extension.empty())
            continue;

        // Both the path and the basename are copied into the response.
        if (!budget.charge(sizeof(Font)) || !budget.charge(bytes) || !budget.charge(bytes)) {
            fail(error, ExtractionBudgetError);
            return { };
        }
        result.push_back(
            { fontNameFromEntry(entry), std::string(entry), fontType(extension), FontEmbedType::FullyEmbedded });
    }

    m_fonts = std::move(result);
    return *m_fonts;
}

} // namespace Mu::Worker::Engine
