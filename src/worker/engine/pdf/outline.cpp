// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/destination.hpp"
#include "engine/pdf/document.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

#include "engine/constants.hpp"
#include "engine/pdf/generated_outline.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Outline / Table-of-Contents Extraction
// =============================================================================

std::vector<OutlineNode> PdfDocument::outline(std::string* error) const
{
    if (!m_document || m_locked) {
        fail(error, "document is unavailable");
        return { };
    }

    fz_outline* volatile root = nullptr;
    std::vector<OutlineNode> result;

    fz_try(m_context)
    {
        // Load hierarchical document outline from Fitz engine
        root = fz_load_outline(m_context, m_document);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }

    std::size_t count = 0;
    try {
        result = copyOutline(root, 0, &count, error);
    } catch (...) {
        fz_drop_outline(m_context, root);
        throw;
    }
    fz_drop_outline(m_context, root);
    if (error && !error->empty())
        return { };

    return result;
}

std::vector<OutlineNode> PdfDocument::generateOutline(std::string* error) const
{
    if (!m_document || m_locked) {
        fail(error, "document is unavailable");
        return { };
    }
    // The synchronous fallback is deliberately bounded. An incomplete scan is
    // an error, never a successfully cached partial outline.
    constexpr int maxPages = 5000;
    constexpr std::size_t maxCharacters = 32U * 1024U * 1024U;
    constexpr std::size_t maxLines = 100'000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    if (m_pageCount > maxPages) {
        fail(error, "resource limit: generated outline page limit exceeded");
        return { };
    }
    std::vector<OutlineLine> lines;
    std::size_t textBytes = 0;
    std::size_t characters = 0;
    for (int pageIndex = 0; pageIndex < m_pageCount; ++pageIndex) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail(error, "resource limit: generated outline time limit exceeded");
            return { };
        }
        fz_page* page = loadPage(pageIndex, error);
        if (!page)
            return { };
        fz_stext_page* volatile text = nullptr;
        fz_device* volatile device = nullptr;
        fz_rect bounds = fz_empty_rect;
        fz_try(m_context)
        {
            bounds = fz_bound_page(m_context, page);
            fz_stext_options options { };
            options.flags = FZ_STEXT_CLIP;
            text = fz_new_stext_page(m_context, bounds);
            device = fz_new_stext_device(m_context, text, &options);
            // Comments and form appearances must not become document headings.
            fz_run_page_contents(m_context, page, device, fz_identity, nullptr);
            fz_close_device(m_context, device);
        }
        fz_always(m_context)
        {
            fz_drop_device(m_context, device);
            fz_drop_page(m_context, page);
        }
        fz_catch(m_context)
        {
            fz_drop_stext_page(m_context, text);
            fail(error, fz_caught_message(m_context));
            return { };
        }
        const auto dropText = [this](fz_stext_page* value) {
            fz_drop_stext_page(m_context, value);
        };
        const std::unique_ptr<fz_stext_page, decltype(dropText)> owner(text, dropText);
        for (auto* block = text->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (auto* line = block->u.t.first_line; line; line = line->next) {
                if (!line->first_char || line->wmode != 0)
                    continue;
                OutlineLine record;
                // Text records retain the dominant glyph style, rather than
                // the maximum size or an all-characters-bold requirement.
                using Style = std::tuple<std::string, int, bool>;
                std::map<Style, std::size_t> styles;
                std::size_t glyphs = 0;
                std::size_t boldGlyphs = 0;
                for (auto* ch = line->first_char; ch; ch = ch->next) {
                    if (++characters > maxCharacters) {
                        fail(error, "resource limit: generated outline text limit exceeded");
                        return { };
                    }
                    if (ch->c < 32 || ch->c > Constant::UnicodeMaxCodePoint
                        || (ch->c >= Constant::UnicodeSurrogateMin && ch->c <= Constant::UnicodeSurrogateMax))
                        continue;
                    char utf8[4];
                    const int bytes = fz_runetochar(utf8, ch->c);
                    record.text.append(utf8, static_cast<std::size_t>(bytes));
                    if (record.text.size() > 4096)
                        break;
                    if (ch->c == ' ' || !ch->font || !std::isfinite(ch->size) || ch->size <= 0 || ch->size >= 1000)
                        continue;
                    const std::string name = fz_font_name(m_context, ch->font);
                    const bool bold = fz_font_is_bold(m_context, ch->font) || name.find("Bold") != std::string::npos
                        || name.find("bold") != std::string::npos;
                    std::string family = name.substr(name.find('+') == std::string::npos ? 0 : name.find('+') + 1);
                    family.resize(std::min(family.size(), family.find('-')));
                    ++styles[{ family,
                               static_cast<int>(std::round(ch->size * 2)),
                               fz_font_is_monospaced(m_context, ch->font) != 0 }];
                    ++glyphs;
                    boldGlyphs += bold ? 1 : 0;
                }
                if (record.text.empty() || record.text.size() > 4096 || styles.empty())
                    continue;
                textBytes += record.text.size();
                if (textBytes > maxCharacters || lines.size() >= maxLines) {
                    fail(error, "resource limit: generated outline line storage limit exceeded");
                    return { };
                }
                const auto dominant = std::max_element(
                    styles.begin(), styles.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
                record.fontFamily = std::get<0>(dominant->first);
                record.size = static_cast<double>(std::get<1>(dominant->first)) / 2;
                record.monospaced = std::get<2>(dominant->first);
                record.boldFraction = static_cast<double>(boldGlyphs) / static_cast<double>(glyphs);
                const double width = bounds.x1 - bounds.x0;
                const double height = bounds.y1 - bounds.y0;
                if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0
                    || !std::isfinite(line->bbox.x0) || !std::isfinite(line->bbox.y0) || !std::isfinite(line->bbox.x1)
                    || !std::isfinite(line->bbox.y1))
                    continue;
                record.viewport.page = pageIndex;
                record.viewport.coordinateMask = Model::Viewport::CoordinateX | Model::Viewport::CoordinateY;
                record.viewport.normalizedX = std::clamp((line->bbox.x0 - bounds.x0) / width, 0.0, 1.0);
                record.viewport.normalizedY =
                    normalizeDestinationY(std::clamp((line->bbox.y0 - bounds.y0) / height, 0.0, 1.0), height);
                // Classify in reading orientation, while retaining the display
                // coordinates for destinations on rotated pages.
                if (!std::isfinite(line->dir.x) || !std::isfinite(line->dir.y)
                    || std::max(std::abs(line->dir.x), std::abs(line->dir.y)) < 0.9f)
                    continue;
                const float angle = std::round(std::atan2(line->dir.y, line->dir.x) * 180.0f / 3.14159265f / 90) * 90;
                const auto reading = fz_rotate(-angle);
                const auto readingBounds = fz_transform_rect(bounds, reading);
                const auto rect = fz_transform_rect(line->bbox, reading);
                record.page = pageIndex;
                record.left = rect.x0 - readingBounds.x0;
                record.top = rect.y0 - readingBounds.y0;
                record.right = rect.x1 - readingBounds.x0;
                record.bottom = rect.y1 - readingBounds.y0;
                record.pageWidth = readingBounds.x1 - readingBounds.x0;
                record.pageHeight = readingBounds.y1 - readingBounds.y0;
                lines.push_back(std::move(record));
            }
        }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        fail(error, "resource limit: generated outline time limit exceeded");
        return { };
    }
    std::vector<OutlineNode> result;
    try {
        result = buildGeneratedOutline(std::move(lines));
    } catch (const std::length_error& exception) {
        fail(error, std::string("resource limit: ") + exception.what());
        return { };
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        fail(error, "resource limit: generated outline time limit exceeded");
        return { };
    }
    return result;
}

std::vector<OutlineNode>
PdfDocument::copyOutline(const fz_outline* source, std::size_t depth, std::size_t* count, std::string* error) const
{
    // Enforce depth limit against maliciously circular or deeply nested outline trees
    if (depth > Constant::MaxOutlineDepth) {
        fail(error, "outline nesting limit exceeded");
        return { };
    }

    std::vector<OutlineNode> result;
    for (const fz_outline* item = source; item; item = item->next) {
        // Enforce total node count limit
        if (++*count > Constant::MaxOutlineNodes) {
            fail(error, "outline node limit exceeded");
            return { };
        }

        OutlineNode node;
        if (item->title)
            node.title = item->title;

        node.open = item->is_open != 0;
        if (item->uri)
            node.link = resolveLink(item->uri, error);
        if (error && !error->empty())
            return { };

        // Recurse into children items
        node.children = copyOutline(item->down, depth + 1, count, error);
        if (error && !error->empty())
            return { };
        result.push_back(std::move(node));
    }
    return result;
}

} // namespace Mu::Worker::Engine
