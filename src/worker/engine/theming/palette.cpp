// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/theming/palette.hpp"
#include "shared/model/theme_colors.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>

namespace Mu::Worker::Engine {
namespace {

using ThemePalette = std::array<std::array<std::uint8_t, 256>, 3>;

struct PageProbe {
    fz_device super;
    ThemePageInfo* info;
};

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
    return settings.readingTheme == Model::ReadingTheme::Default ? settings.paperColorRgb
                                                                 : Model::ThemeColor::DefaultBackground;
}

bool isMostlyDarkPixmap(fz_context* context, fz_pixmap* pixmap)
{
    if (fz_pixmap_components(context, pixmap) != 4)
        fz_throw(context, FZ_ERROR_ARGUMENT, "reading theme requires an RGBA viewing pixmap");
    const int width = fz_pixmap_width(context, pixmap);
    const int height = fz_pixmap_height(context, pixmap);
    const int stride = fz_pixmap_stride(context, pixmap);
    const auto* samples = fz_pixmap_samples(context, pixmap);
    std::size_t darkPixels = 0;
    for (int y = 0; y < height; ++y) {
        const auto* pixel = samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        std::uint32_t darkRow = 0;
        for (int x = 0; x < width; ++x, pixel += 4) {
            // A packed, alignment-safe load lets baseline SSE2 vectorize the reduction.
            std::uint32_t rgba;
            std::memcpy(&rgba, pixel, sizeof(rgba));
            if constexpr (std::endian::native == std::endian::big)
                rgba = std::byteswap(rgba);
            // Approximate perceptual luminance; the maximum sum is 255 * 256.
            const auto luminance =
                static_cast<std::uint16_t>(54 * (rgba & 255) + 183 * ((rgba >> 8) & 255) + 19 * ((rgba >> 16) & 255));
            darkRow += luminance < 128 * 256;
        }
        darkPixels += darkRow;
    }
    return darkPixels > static_cast<std::size_t>(width) * static_cast<std::size_t>(height) / 2;
}

// Always sample the whole page, independently of the requested tile or zoom.
// Keep the thumbnail temporary so edits and layer changes cannot leave stale decisions.
ThemePageInfo
inspectThemePage(fz_context* context, fz_page* page, fz_rect bounds, fz_cookie* cookie, fz_display_list* list)
{
    fz_pixmap* volatile thumbnail = nullptr;
    fz_device* volatile device = nullptr;
    ThemePageInfo info;
    fz_var(info);
    fz_try(context)
    {
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        thumbnail = fz_new_pixmap(context, fz_device_rgb(context), 64, 64, nullptr, 1);
        fz_clear_pixmap_with_value(context, thumbnail, 255);
        const fz_matrix transform = fz_concat(fz_translate(-bounds.x0, -bounds.y0),
                                              fz_scale(64 / (bounds.x1 - bounds.x0), 64 / (bounds.y1 - bounds.y0)));
        device = fz_new_draw_device(context, transform, thumbnail);
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
            state->info->needsCompositing |= (blendmode & FZ_BLEND_MODEMASK) != FZ_BLEND_NORMAL || knockout != 0;
            fz_begin_group(ctx, dev->passthrough, area, cs, isolated, knockout, blendmode, alpha);
        };
        fz_drop_device(context, device);
        device = &probe->super;
        if (list)
            fz_run_display_list(context, list, device, fz_identity, fz_infinite_rect, cookie);
        else
            fz_run_page(context, page, device, fz_identity, cookie);
        fz_close_device(context, device);
        if (cookie && cookie->abort)
            fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
        info.mostlyDark = isMostlyDarkPixmap(context, thumbnail);
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
        const auto red = ((rgba & 255) * 4 + 2) / 5;
        const auto green = (((rgba >> 8) & 255) * 4 + 2) / 5;
        const auto blue = (((rgba >> 16) & 255) * 4 + 2) / 5;
        rgba = (rgba & 0xFF000000U) | red | (green << 8) | (blue << 16);
        if constexpr (std::endian::native == std::endian::big)
            rgba = std::byteswap(rgba);
        std::memcpy(pixel, &rgba, sizeof(rgba));
    }
}

// Transform an RGBA viewing pixmap, preserving premultiplied alpha.
void applyDocumentTheme(fz_context* context, fz_pixmap* pixmap, Model::ReadingTheme theme, const fz_cookie* cookie)
{
    // Dusk is applied by its image-preserving rendering device.
    if (theme == Model::ReadingTheme::Default || theme == Model::ReadingTheme::Dusk)
        return;
    if (fz_pixmap_components(context, pixmap) != 4)
        fz_throw(context, FZ_ERROR_ARGUMENT, "reading theme requires an RGBA viewing pixmap");
    std::array<int, 3> background;
    std::array<int, 3> foreground;
    bool invertBrightness = false;
    switch (theme) {
    case Model::ReadingTheme::Twilight:
        background = Model::ThemeColor::splitRgb(Model::ThemeColor::TwilightBackground);
        foreground = Model::ThemeColor::splitRgb(Model::ThemeColor::TwilightForeground);
        break;
    case Model::ReadingTheme::Dark:
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
                red = low + (high - red) * 2 / 5;
                green = low + (high - green) * 2 / 5;
                blue = low + (high - blue) * 2 / 5;
            }
            pixel[0] = static_cast<unsigned char>((palette[0][static_cast<std::size_t>(red)] * alpha + 127) / 255);
            pixel[1] = static_cast<unsigned char>((palette[1][static_cast<std::size_t>(green)] * alpha + 127) / 255);
            pixel[2] = static_cast<unsigned char>((palette[2][static_cast<std::size_t>(blue)] * alpha + 127) / 255);
        }
    }
}

} // namespace Mu::Worker::Engine
