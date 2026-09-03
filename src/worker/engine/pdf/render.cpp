// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"
#include "shared/model/validation.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Page Rasterization & Tile Rendering
// =============================================================================

bool PdfDocument::renderToBuffer(const RenderRequest& request,
                                 void* dstPixels,
                                 std::size_t dstStride,
                                 std::string* error) const
{
    // The caller owns dstPixels. MuPDF owns the temporary page, pixmap, and
    // device handles until the fz_always cleanup below completes.
    if (!dstPixels)
        return fail(error, "destination buffer is null");

    const int width = request.width;
    const int height = request.height;
    if (request.page < 0 || request.page >= m_pageCount
        || !isValidRenderDimensions(width, height, request.tile.has_value()))
        return fail(error, "render dimensions are invalid");

    // Validate tile geometry against target dimensions
    const RenderTile tile = request.tile.value_or({ 0, 0, width, height });
    if (!isValidRenderTile(width, height, tile.x, tile.y, tile.width, tile.height))
        return fail(error, "render tile is outside the requested image");

    const std::size_t minRowBytes = static_cast<std::size_t>(tile.width) * 4;
    if (dstStride < minRowBytes)
        return fail(error, "destination stride is too small");

    fz_page* nativePage = loadPage(request.page, error);
    if (!nativePage)
        return false;

    fz_pixmap* pixmap = nullptr;
    fz_device* device = nullptr;

    fz_var(pixmap);
    fz_var(device);

    fz_try(m_context)
    {
        const fz_rect bounds = fz_bound_page(m_context, nativePage);
        const float pageWidth = bounds.x1 - bounds.x0;
        const float pageHeight = bounds.y1 - bounds.y0;
        if (pageWidth <= 0 || pageHeight <= 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "page has invalid bounds");

        const bool tiled = request.tile.has_value();
        // 1-pixel bleed around tile boundaries avoids interpolation edge seam artifacts
        const fz_irect bbox {
            tiled ? std::max(0, tile.x - Constant::TileBleed) : 0,
            tiled ? std::max(0, tile.y - Constant::TileBleed) : 0,
            tiled ? std::min(width, tile.x + tile.width + Constant::TileBleed) : width,
            tiled ? std::min(height, tile.y + tile.height + Constant::TileBleed) : height,
        };

        const fz_matrix transform =
            pageToDevice(bounds, static_cast<float>(width) / pageWidth, static_cast<float>(height) / pageHeight);

        // Optimization: When rendering a full page directly into a matching buffer stride,
        // point MuPDF's pixmap memory directly at destination memory (zero-copy rendering).
        if (!tiled && dstStride == minRowBytes) {
            pixmap = fz_new_pixmap_with_data(m_context,
                                             fz_device_rgb(m_context),
                                             width,
                                             height,
                                             nullptr,
                                             1,
                                             static_cast<int>(dstStride),
                                             static_cast<unsigned char*>(dstPixels));
        } else {
            pixmap = fz_new_pixmap_with_bbox(m_context, fz_device_rgb(m_context), bbox, nullptr, 1);
        }

        // Initialize background with the opaque Okular paper color.
        float paper[3] = { ((m_settings.paperColorRgb >> 16) & 0xFF) / 255.0f,
                           ((m_settings.paperColorRgb >> 8) & 0xFF) / 255.0f,
                           (m_settings.paperColorRgb & 0xFF) / 255.0f };
        fz_fill_pixmap_with_color(m_context, pixmap, fz_device_rgb(m_context), paper, fz_default_color_params);
        device = fz_new_draw_device(m_context, tiled ? transform : fz_identity, pixmap);

        if (!m_settings.interpolateImages)
            fz_enable_device_hints(m_context, device, FZ_DONT_INTERPOLATE_IMAGES);

        fz_cookie cookie { 0, 0, 0, 0, 0 };
        fz_run_page(m_context, nativePage, device, tiled ? fz_identity : transform, &cookie);

        fz_close_device(m_context, device);

        // Blit sub-tile region into the destination frame buffer with specified row stride
        if (tiled || dstStride != minRowBytes) {
            const int stride = fz_pixmap_stride(m_context, pixmap);
            const auto* samples = fz_pixmap_samples(m_context, pixmap);
            const int offsetX = tiled ? tile.x - bbox.x0 : 0;
            const int offsetY = tiled ? tile.y - bbox.y0 : 0;

            auto* dst = static_cast<std::uint8_t*>(dstPixels);
            for (int row = 0; row < tile.height; ++row) {
                std::memcpy(dst + static_cast<std::size_t>(row) * dstStride,
                            samples
                                + (static_cast<std::size_t>(row) + static_cast<std::size_t>(offsetY))
                                    * static_cast<std::size_t>(stride)
                                + static_cast<std::size_t>(offsetX) * 4,
                            minRowBytes);
            }
        }
    }
    fz_always(m_context)
    {
        if (device)
            fz_drop_device(m_context, device);
        if (pixmap)
            fz_drop_pixmap(m_context, pixmap);
        fz_drop_page(m_context, nativePage);
    }
    fz_catch(m_context)
    {
        return fail(error, fz_caught_message(m_context));
    }

    return true;
}

// =============================================================================
// Text Box Extraction
// =============================================================================

std::vector<TextBox> PdfDocument::textBoxes(
    int page, double dpiX, double dpiY, std::size_t maxBoxes, bool skipAnnots, std::string* error) const
{
    if (!isValidDpi(dpiX, dpiY)) {
        fail(error, "text DPI is invalid");
        return { };
    }

    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return { };

    fz_stext_page* text = nullptr;
    fz_device* device = nullptr;
    std::vector<TextBox> result;

    fz_var(text);
    fz_var(device);

    fz_try(m_context)
    {
        const fz_rect bounds = fz_bound_page(m_context, nativePage);
        const fz_matrix transform =
            pageToDevice(bounds, static_cast<float>(dpiX / 72.0), static_cast<float>(dpiY / 72.0));

        fz_stext_options options { };
        options.flags = FZ_STEXT_CLIP;

        // Build structured text page tree
        text = fz_new_stext_page(m_context, bounds);
        device = fz_new_stext_device(m_context, text, &options);

        fz_cookie cookie { 0, 0, 0, 0, 0 };
        if (skipAnnots)
            fz_run_page_contents(m_context, nativePage, device, fz_identity, &cookie);
        else
            fz_run_page(m_context, nativePage, device, fz_identity, &cookie);

        fz_close_device(m_context, device);
        fz_drop_device(m_context, device);
        device = nullptr;

        const std::size_t charCount = countStextChars(text);
        if (charCount > 0)
            result.reserve(std::min(charCount, maxBoxes));

        // Traverse hierarchy: blocks -> lines -> characters
        for (fz_stext_block* block = text->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;

            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                bool hasText = false;
                for (fz_stext_char* character = line->first_char; character; character = character->next) {
                    // Filter invalid Unicode code points and surrogates
                    if (character->c < 0 || character->c > 0x10ffff
                        || (character->c >= 0xd800 && character->c <= 0xdfff))
                        continue;

                    // Clip to visible page boundaries
                    const fz_rect charBounds = fz_rect_from_quad(character->quad);
                    if (charBounds.x1 <= bounds.x0 || charBounds.x0 >= bounds.x1 || charBounds.y1 <= bounds.y0
                        || charBounds.y0 >= bounds.y1)
                        continue;

                    // Convert unicode codepoint to UTF-8
                    char utf8[4] { };
                    const int bytes = fz_runetochar(utf8, character->c);
                    if (bytes <= 0)
                        continue;

                    if (result.size() >= maxBoxes)
                        fz_throw(m_context, FZ_ERROR_GENERIC, "text box limit exceeded");

                    // Transform character bounding box to target DPI device coordinates
                    const fz_rect rect = fz_transform_rect(charBounds, transform);
                    result.emplace_back(
                        std::string(utf8, static_cast<std::size_t>(bytes)), rect.x0, rect.y0, rect.x1, rect.y1, false);
                    hasText = true;
                }
                if (hasText)
                    result.back().endOfLine = true;
            }
        }
    }
    fz_always(m_context)
    {
        if (device) {
            fz_try(m_context)
            {
                fz_close_device(m_context, device);
            }
            fz_catch(m_context)
            {
            }
            fz_drop_device(m_context, device);
        }
        if (text)
            fz_drop_stext_page(m_context, text);
        fz_drop_page(m_context, nativePage);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }

    return result;
}

// =============================================================================
// Coordinate Matrix Transformations
// =============================================================================

fz_matrix PdfDocument::pageToDevice(const fz_rect& bounds, float scaleX, float scaleY) noexcept
{
    return fz_concat(fz_translate(-bounds.x0, -bounds.y0), fz_scale(scaleX, scaleY));
}

} // namespace Mu::Worker::Engine
