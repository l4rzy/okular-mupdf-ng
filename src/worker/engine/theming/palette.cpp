// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/theming/palette.hpp"
#include "shared/model/theme_colors.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <mupdf/pdf.h>

namespace Mu::Worker::Engine {
namespace {

using ThemePalette = std::array<std::array<std::uint8_t, 256>, 3>;

struct PageProbe {
    fz_device super;
    ThemePageInfo* info;
    bool highlight;
};

bool isDarkPixel(const unsigned char* pixel) noexcept
{
    // Packed, alignment-safe loads allow baseline SSE2 to vectorize reductions.
    std::uint32_t rgba;
    std::memcpy(&rgba, pixel, sizeof(rgba));
    if constexpr (std::endian::native == std::endian::big)
        rgba = std::byteswap(rgba);
    const auto red = rgba & 0xFF;
    const auto green = (rgba >> 8) & 0xFF;
    const auto blue = (rgba >> 16) & 0xFF;
    // Approximate perceptual luminance with integer weights that sum to 256.
    const auto luminance = Theme::RedWeight * red + Theme::GreenWeight * green + Theme::BlueWeight * blue;
    return luminance < Theme::PageDarknessCutoff * Theme::LuminanceScale;
}

bool hasMostlyDarkBorder(fz_context* context, fz_pixmap* pixmap)
{
    // The page probe supplies an opaque RGBA thumbnail.
    const int width = fz_pixmap_width(context, pixmap);
    const int height = fz_pixmap_height(context, pixmap);
    if (width < 2 || height < 2)
        return false;
    const auto stride = static_cast<std::size_t>(fz_pixmap_stride(context, pixmap));
    const auto* top = fz_pixmap_samples(context, pixmap);
    const auto* bottom = top + static_cast<std::size_t>(height - 1) * stride;
    std::size_t darkPixels = 0;
    for (int x = 0; x < width; ++x)
        darkPixels += isDarkPixel(top + x * 4) + isDarkPixel(bottom + x * 4);
    for (int y = 1; y < height - 1; ++y) {
        const auto* row = top + static_cast<std::size_t>(y) * stride;
        darkPixels += isDarkPixel(row) + isDarkPixel(row + (width - 1) * 4);
    }
    // Count corners once and require a strict majority, as in the full-page scan.
    const auto borderPixels = 2 * (static_cast<std::size_t>(width) + static_cast<std::size_t>(height)) - 4;
    return darkPixels > borderPixels / Theme::DarkCoverageDivisor;
}

ThemePalette makeThemePalette(const std::array<int, 3>& background, const std::array<int, 3>& foreground) noexcept
{
    ThemePalette palette { };
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const int start = foreground[channel];
        const int range = background[channel] - start;
        for (std::size_t value = 0; value < 256; ++value)
            palette[channel][value] = static_cast<std::uint8_t>(start + range * static_cast<int>(value) / 255);
    }
    return palette;
}

} // namespace

std::uint32_t readingPaperColor(const Model::DocumentSettings& settings) noexcept
{
    return settings.documentTheme == Model::DocumentTheme::Default ? settings.paperColorRgb
                                                                   : Model::ThemeColor::DefaultBackground;
}

ThemeRenderPlan planThemeRendering(const Model::DocumentSettings& settings, ThemePageInfo page) noexcept
{
    const auto theme = settings.documentTheme;
    const bool deviceTheme = shouldInspectThemePage(theme);
    const bool recolorVectors = deviceTheme && !page.mostlyDark;
    // Dark recoloring uses the completed bitmap when drawing operations
    // need compositing, so overlapping operations are themed together.
    const bool bitmapDark = theme == Model::DocumentTheme::Dark && recolorVectors && page.needsCompositing;
    const auto paperColor =
        recolorVectors && !bitmapDark ? Model::ThemeColor::getBackground(theme) : readingPaperColor(settings);
    return { deviceTheme && !bitmapDark, recolorVectors, paperColor };
}

bool isMostlyDarkPixmap(fz_context* context, fz_pixmap* pixmap)
{
    if (fz_pixmap_components(context, pixmap) != 4)
        fz_throw(context, FZ_ERROR_ARGUMENT, "document theme requires an RGBA viewing pixmap");
    const int width = fz_pixmap_width(context, pixmap);
    const int height = fz_pixmap_height(context, pixmap);
    const int stride = fz_pixmap_stride(context, pixmap);
    const auto* samples = fz_pixmap_samples(context, pixmap);
    std::size_t darkPixels = 0;
    for (int y = 0; y < height; ++y) {
        const auto* pixel = samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        std::uint32_t darkRow = 0;
        for (int x = 0; x < width; ++x, pixel += 4)
            darkRow += isDarkPixel(pixel);
        darkPixels += darkRow;
    }
    return darkPixels > static_cast<std::size_t>(width) * static_cast<std::size_t>(height) / Theme::DarkCoverageDivisor;
}

// Dusk samples the whole page, independently of the requested tile or zoom.
// Keep the thumbnail temporary so edits and layer changes cannot leave stale decisions.
ThemePageInfo inspectThemePage(fz_context* context,
                               fz_page* page,
                               fz_rect bounds,
                               Model::DocumentTheme theme,
                               fz_cookie* cookie,
                               fz_display_list* list)
{
    fz_pixmap* volatile thumbnail = nullptr;
    fz_device* volatile device = nullptr;
    ThemePageInfo info;
    fz_var(info);
    fz_try(context)
    {
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        if (theme == Model::DocumentTheme::Dusk) {
            thumbnail =
                fz_new_pixmap(context, fz_device_rgb(context), Theme::ThumbnailSize, Theme::ThumbnailSize, nullptr, 1);
            fz_clear_pixmap_with_value(context, thumbnail, 255);
            const fz_matrix transform = fz_concat(fz_translate(-bounds.x0, -bounds.y0),
                                                  fz_scale(Theme::ThumbnailSize / (bounds.x1 - bounds.x0),
                                                           Theme::ThumbnailSize / (bounds.y1 - bounds.y0)));
            device = fz_new_draw_device(context, transform, thumbnail);
        } else {
            // Dark only needs blend inspection; avoid rasterizing a page thumbnail.
            device = fz_new_device_of_size(context, sizeof(fz_device));
        }
        auto* probe =
            reinterpret_cast<PageProbe*>(fz_new_passthrough_device_of_size(context, device, sizeof(PageProbe)));
        probe->info = &info;
        probe->super.begin_group = [](fz_context* ctx,
                                      fz_device* dev,
                                      fz_rect area,
                                      fz_colorspace* cs,
                                      int isolated,
                                      int knockout,
                                      int blendmode,
                                      float alpha) {
            // Non-isolated groups can depend on everything painted before them.
            // Their original paints must be composited before recoloring.
            auto* state = reinterpret_cast<PageProbe*>(dev);
            const int mode = blendmode & FZ_BLEND_MODEMASK;
            // Highlights need Multiply against the themed backdrop to retain text contrast.
            state->info->needsCompositing |=
                knockout != 0 || (mode != FZ_BLEND_NORMAL && !(state->highlight && mode == FZ_BLEND_MULTIPLY));
            fz_begin_group(ctx, dev->passthrough, area, cs, isolated, knockout, blendmode, alpha);
        };
        fz_drop_device(context, device);
        device = &probe->super;
        if (list)
            fz_run_display_list(context, list, device, fz_identity, fz_infinite_rect, cookie);
        else if (auto* pdfPage = pdf_page_from_fz_page(context, page)) {
            fz_run_page_contents(context, page, device, fz_identity, cookie);
            // Inspect each annotation in its original order so only highlights
            // receive the exception; custom annotation blend modes remain protected.
            fz_try(context)
            {
                pdf_update_page(context, pdfPage);
                for (auto* annot = pdf_first_annot(context, pdfPage); annot; annot = pdf_next_annot(context, annot)) {
                    if (cookie && cookie->abort)
                        fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
                    probe->highlight = pdf_annot_type(context, annot) == PDF_ANNOT_HIGHLIGHT;
                    pdf_run_annot(context, annot, device, fz_identity, cookie);
                }
            }
            fz_catch(context)
            {
                // Match fz_run_page_annots' handling of malformed appearances and cancellation.
                device->close_device = nullptr;
                fz_rethrow_unless(context, FZ_ERROR_ABORT);
                fz_ignore_error(context);
            }
            probe->highlight = false;
            fz_run_page_widgets(context, page, device, fz_identity, cookie);
        } else
            fz_run_page(context, page, device, fz_identity, cookie);
        fz_close_device(context, device);
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        // Require dark edges too: a large inset photograph can dominate a light page.
        info.mostlyDark =
            thumbnail && isMostlyDarkPixmap(context, thumbnail) && hasMostlyDarkBorder(context, thumbnail);
    }
    fz_always(context)
    {
        fz_drop_device(context, device);
        fz_drop_pixmap(context, thumbnail);
    }
    fz_catch(context) fz_rethrow(context);
    return info;
}

// Dim premultiplied RGBA to 80%, retaining alpha. Packed, alignment-safe
// loads/stores allow baseline x86-64 compilers to vectorize across pixels.
void dimImageRow(unsigned char* pixel, int width) noexcept
{
    for (int x = 0; x < width; ++x, pixel += 4) {
        std::uint32_t rgba;
        std::memcpy(&rgba, pixel, sizeof(rgba));
        if constexpr (std::endian::native == std::endian::big)
            rgba = std::byteswap(rgba);
        const auto red = ((rgba & 255) * Theme::ImageBrightnessNumerator + Theme::ImageBrightnessDenominator / 2)
            / Theme::ImageBrightnessDenominator;
        const auto green =
            (((rgba >> 8) & 255) * Theme::ImageBrightnessNumerator + Theme::ImageBrightnessDenominator / 2)
            / Theme::ImageBrightnessDenominator;
        const auto blue =
            (((rgba >> 16) & 255) * Theme::ImageBrightnessNumerator + Theme::ImageBrightnessDenominator / 2)
            / Theme::ImageBrightnessDenominator;
        rgba = (rgba & 0xFF000000U) | red | (green << 8) | (blue << 16);
        if constexpr (std::endian::native == std::endian::big)
            rgba = std::byteswap(rgba);
        std::memcpy(pixel, &rgba, sizeof(rgba));
    }
}

// Transform an RGBA viewing pixmap, preserving premultiplied alpha.
void applyDocumentTheme(fz_context* context, fz_pixmap* pixmap, Model::DocumentTheme theme, const fz_cookie* cookie)
{
    // Dusk is applied by its image-preserving rendering device.
    if (theme == Model::DocumentTheme::Default || theme == Model::DocumentTheme::Dusk)
        return;
    if (fz_pixmap_components(context, pixmap) != 4)
        fz_throw(context, FZ_ERROR_ARGUMENT, "document theme requires an RGBA viewing pixmap");
    std::array<int, 3> background;
    std::array<int, 3> foreground;
    bool invertBrightness = false;
    switch (theme) {
    case Model::DocumentTheme::Twilight:
        background = Model::ThemeColor::splitRgb(Model::ThemeColor::TwilightBackground);
        foreground = Model::ThemeColor::splitRgb(Model::ThemeColor::TwilightForeground);
        break;
    case Model::DocumentTheme::Dark:
        background = Model::ThemeColor::splitRgb(Model::ThemeColor::DarkBackground);
        foreground = Model::ThemeColor::splitRgb(Model::ThemeColor::DarkForeground);
        invertBrightness = true;
        break;
    default:
        return;
    }
    const auto palette = makeThemePalette(background, foreground);
    const int width = fz_pixmap_width(context, pixmap);
    const int height = fz_pixmap_height(context, pixmap);
    const int stride = fz_pixmap_stride(context, pixmap);
    auto* samples = fz_pixmap_samples(context, pixmap);
    for (int y = 0; y < height; ++y) {
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        auto* pixel = samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < width; ++x, pixel += 4) {
            const int alpha = pixel[3];
            if (!alpha)
                continue;
            int red = pixel[0], green = pixel[1], blue = pixel[2];
            if (alpha != 255) {
                red = std::min(255, red * 255 / alpha);
                green = std::min(255, green * 255 / alpha);
                blue = std::min(255, blue * 255 / alpha);
            }
            if (invertBrightness) {
                // Invert brightness and soften saturation to keep colors readable.
                // A fixed blend preserves antialiased coverage against neutral paper.
                const int low = std::min({ red, green, blue });
                const int high = std::max({ red, green, blue });
                red = low + (high - red) * Theme::SaturationNumerator / Theme::SaturationDenominator;
                green = low + (high - green) * Theme::SaturationNumerator / Theme::SaturationDenominator;
                blue = low + (high - blue) * Theme::SaturationNumerator / Theme::SaturationDenominator;
            }
            pixel[0] = static_cast<unsigned char>((palette[0][static_cast<std::size_t>(red)] * alpha + 127) / 255);
            pixel[1] = static_cast<unsigned char>((palette[1][static_cast<std::size_t>(green)] * alpha + 127) / 255);
            pixel[2] = static_cast<unsigned char>((palette[2][static_cast<std::size_t>(blue)] * alpha + 127) / 255);
        }
    }
}

} // namespace Mu::Worker::Engine
