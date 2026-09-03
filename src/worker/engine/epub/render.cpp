// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/epub/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "shared/model/validation.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Page Rasterization & Tile Rendering
// =============================================================================

bool EpubDocument::renderToBuffer(const RenderRequest& request,
                                  void* dstPixels,
                                  std::size_t dstStride,
                                  std::string* error) const
{
    // The caller owns dstPixels. This function owns the loaded page and MuPDF
    // rendering handles until the fz_always cleanup below completes.
    if (!dstPixels)
        return fail(error, "destination buffer is null");
    if (!m_document || request.width <= 0 || request.height <= 0)
        return fail(error, "invalid render target dimensions");

    const int targetWidth = request.tile ? request.tile->width : request.width;
    const int targetHeight = request.tile ? request.tile->height : request.height;
    const std::size_t minRowBytes = static_cast<std::size_t>(targetWidth) * 4;
    if (dstStride < minRowBytes)
        return fail(error, "destination stride is too small");

    if (request.tile
        && !isValidRenderTile(
            request.width, request.height, request.tile->x, request.tile->y, request.tile->width, request.tile->height))
        return fail(error, "render tile is invalid");

    fz_rect bounds { };
    fz_page* page = loadPageWithBounds(request.page, &bounds, error);
    if (!page)
        return false;

    fz_matrix ctm = fz_identity;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;

    fz_var(pix);
    fz_var(dev);

    fz_try(m_context)
    {
        const float pageWidth = bounds.x1 - bounds.x0;
        const float pageHeight = bounds.y1 - bounds.y0;
        const auto layout = layoutGeometry();
        if (pageWidth <= 0 || pageHeight <= 0 || layout.paperWidth <= 0 || layout.paperHeight <= 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "page has invalid bounds");

        // Treat the requested output as a zoomed view of the fixed page. Keep
        // one scale factor on both axes so an unusual aspect-ratio request
        // produces letterboxing instead of stretching the page.
        const float scale = std::min(static_cast<float>(request.width) / layout.paperWidth,
                                     static_cast<float>(request.height) / layout.paperHeight);
        const float offsetX = (static_cast<float>(request.width) - layout.paperWidth * scale) * 0.5f;
        const float offsetY = (static_cast<float>(request.height) - layout.paperHeight * scale) * 0.5f;
        ctm =
            fz_concat(fz_translate(-bounds.x0 + offsetX / scale, -bounds.y0 + offsetY / scale), fz_scale(scale, scale));

        fz_irect bbox { 0, 0, request.width, request.height };
        if (request.tile) {
            bbox.x0 = request.tile->x;
            bbox.y0 = request.tile->y;
            bbox.x1 = request.tile->x + request.tile->width;
            bbox.y1 = request.tile->y + request.tile->height;
        }

        const bool tiled = request.tile.has_value();
        // Render directly into the caller's buffer whenever its stride is
        // tight. This covers both full pages and tiles; padded strides retain
        // the temporary-pixmap fallback below.
        if (dstStride == minRowBytes) {
            if (tiled) {
                pix = fz_new_pixmap_with_bbox_and_data(
                    m_context, fz_device_rgb(m_context), bbox, nullptr, 1, static_cast<unsigned char*>(dstPixels));
            } else {
                pix = fz_new_pixmap_with_data(m_context,
                                              fz_device_rgb(m_context),
                                              request.width,
                                              request.height,
                                              nullptr,
                                              1,
                                              static_cast<int>(dstStride),
                                              static_cast<unsigned char*>(dstPixels));
            }
        } else {
            pix = fz_new_pixmap_with_bbox(m_context, fz_device_rgb(m_context), bbox, nullptr, 1);
        }

        // Initialize background with the opaque Okular paper color.
        float paper[3] = { ((m_settings.paperColorRgb >> 16) & 0xFF) / 255.0f,
                           ((m_settings.paperColorRgb >> 8) & 0xFF) / 255.0f,
                           (m_settings.paperColorRgb & 0xFF) / 255.0f };
        fz_fill_pixmap_with_color(m_context, pix, fz_device_rgb(m_context), paper, fz_default_color_params);
        dev = fz_new_draw_device(m_context, ctm, pix);
        fz_run_page(m_context, page, dev, fz_identity, nullptr);
        fz_close_device(m_context, dev);
        fz_drop_device(m_context, dev);
        dev = nullptr;

        // Blit only when the temporary-pixmap fallback was required.
        if (dstStride != minRowBytes) {
            const int pWidth = fz_pixmap_width(m_context, pix);
            const int pHeight = fz_pixmap_height(m_context, pix);
            const int pStride = fz_pixmap_stride(m_context, pix);
            const unsigned char* samples = fz_pixmap_samples(m_context, pix);
            const int n = fz_pixmap_components(m_context, pix);

            auto* dstBase = static_cast<unsigned char*>(dstPixels);
            const int copyWidth = std::min(targetWidth, pWidth);
            const int copyHeight = std::min(targetHeight, pHeight);
            if (n == 4) {
                const std::size_t copyRowBytes = static_cast<std::size_t>(copyWidth) * 4U;
                for (int y = 0; y < copyHeight; ++y) {
                    const unsigned char* srcRow =
                        samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(pStride);
                    unsigned char* dstRow = dstBase + static_cast<std::size_t>(y) * dstStride;
                    std::memcpy(dstRow, srcRow, copyRowBytes);
                }
            } else {
                for (int y = 0; y < copyHeight; ++y) {
                    const unsigned char* srcRow =
                        samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(pStride);
                    unsigned char* dstRow = dstBase + static_cast<std::size_t>(y) * dstStride;
                    for (int x = 0; x < copyWidth; ++x) {
                        const unsigned char* src = srcRow + x * n;
                        unsigned char* dst = dstRow + x * 4;
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = (n >= 4) ? src[3] : 255;
                    }
                }
            }
        }
    }
    fz_always(m_context)
    {
        if (dev)
            fz_drop_device(m_context, dev);
        if (pix)
            fz_drop_pixmap(m_context, pix);
        if (page)
            fz_drop_page(m_context, page);
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

std::vector<TextBox>
EpubDocument::textBoxes(int page, double dpiX, double dpiY, std::size_t maxBoxes, bool, std::string* error) const
{
    if (!isValidDpi(dpiX, dpiY)) {
        fail(error, "text DPI is invalid");
        return { };
    }

    fz_rect bounds { };
    fz_page* pagePtr = loadPageWithBounds(page, &bounds, error);
    if (!pagePtr)
        return { };

    std::vector<TextBox> boxes;
    fz_stext_page* stext = nullptr;
    fz_var(stext);
    fz_try(m_context)
    {
        const double scaleX = dpiX / 72.0;
        const double scaleY = dpiY / 72.0;
        fz_stext_options options { };
        options.flags = FZ_STEXT_CLIP;
        stext = fz_new_stext_page_from_page(m_context, pagePtr, &options);

        const std::size_t charCount = countStextChars(stext);
        if (charCount > 0)
            boxes.reserve(std::min(charCount, maxBoxes));

        for (fz_stext_block* block = stext->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                bool hasText = false;
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                    if (ch->c < 0 || ch->c > 0x10ffff || (ch->c >= 0xd800 && ch->c <= 0xdfff))
                        continue;

                    const fz_rect charBox = fz_rect_from_quad(ch->quad);
                    if (charBox.x1 <= bounds.x0 || charBox.x0 >= bounds.x1 || charBox.y1 <= bounds.y0
                        || charBox.y0 >= bounds.y1)
                        continue;

                    char utf8[8] = { 0 };
                    const int len = fz_runetochar(utf8, ch->c);
                    if (len <= 0)
                        continue;
                    if (boxes.size() >= maxBoxes)
                        fz_throw(m_context, FZ_ERROR_GENERIC, "text box limit exceeded");

                    // TextBox coordinates are device pixels throughout the
                    // worker API. The plugin normalizes them against the
                    // Okular page dimensions, so do not return 0..1
                    // coordinates here.
                    const double left = (charBox.x0 - bounds.x0) * scaleX;
                    const double top = (charBox.y0 - bounds.y0) * scaleY;
                    const double right = (charBox.x1 - bounds.x0) * scaleX;
                    const double bottom = (charBox.y1 - bounds.y0) * scaleY;
                    boxes.emplace_back(
                        std::string(utf8, static_cast<std::size_t>(len)), left, top, right, bottom, false);
                    hasText = true;
                }
                if (hasText)
                    boxes.back().endOfLine = true;
            }
        }
    }
    fz_always(m_context)
    {
        if (stext)
            fz_drop_stext_page(m_context, stext);
        if (pagePtr)
            fz_drop_page(m_context, pagePtr);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    return boxes;
}

} // namespace Mu::Worker::Engine
