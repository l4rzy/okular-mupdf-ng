// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/epub/document.hpp"

#include <algorithm>
#include <cctype>
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
        && suffix != ".woff2")
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
    if (extension == ".ttf" || extension == ".ttc")
        return FontType::TrueType;
    if (extension == ".otf" || extension == ".otc" || extension == ".woff" || extension == ".woff2")
        return FontType::TrueTypeOT;
    return FontType::Unknown;
}

} // namespace

// =============================================================================
// Embedded Font Enumeration
// =============================================================================

std::vector<Font> EpubDocument::fonts(const std::vector<int>&, std::string*) const
{
    if (m_fonts)
        return *m_fonts;

    m_fonts.emplace();
    if (!m_context || !m_stream)
        return *m_fonts;

    fz_try(m_context)
    {
        // Open EPUB ZIP container as a Fitz archive
        if (!m_archive)
            m_archive = fz_try_open_archive_with_stream(m_context, m_stream);
        if (m_archive) {
            const int entryCount = fz_count_archive_entries(m_context, m_archive);
            for (int index = 0; index < entryCount; ++index) {
                const char* entryName = fz_list_archive_entry(m_context, m_archive, index);
                if (!entryName)
                    continue;

                const std::string_view entry(entryName);
                const std::string extension = fontExtension(entry);
                if (extension.empty())
                    continue;

                m_fonts->push_back({ fontNameFromEntry(entry),
                                     std::string(entry),
                                     fontType(extension),
                                     FontEmbedType::FullyEmbedded });
            }
        }
    }
    fz_catch(m_context)
    {
        m_fonts->clear();
    }

    return *m_fonts;
}

} // namespace Mu::Worker::Engine
