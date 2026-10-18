// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/theming/theme_device.hpp"
#include "engine/theming/palette.hpp"
#include "shared/model/theme_colors.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "shared/protocol/limits.hpp"

namespace Mu::Worker::Engine {
namespace {

struct ThemeDevice {
    fz_device super;
    fz_matrix transform;
    const fz_cookie* cookie;
    fz_default_colorspaces* defaultColorspaces;
    Model::ReadingTheme theme;
    std::array<int, 3> background;
    bool recolorVectors;
    int maskDepth;
    std::size_t tileDepth;
    std::array<fz_irect, Limit::MaxDepth + 1> bounds;
};

void checkCancellation(fz_context* context, const ThemeDevice* device)
{
    if (device->cookie && device->cookie->abort)
        fz_throw(context, FZ_ERROR_ABORT, "render cancelled");
}

void mapColor(float* rgb, const std::array<int, 3>& background)
{
    constexpr auto foreground = Model::ThemeColor::splitRgb(Model::ThemeColor::DarkForeground);
    for (int i = 0; i < 3; ++i)
        rgb[i] = std::isfinite(rgb[i]) ? std::clamp(rgb[i], 0.0f, 1.0f) : 0.0f;
    const float low = std::min({ rgb[0], rgb[1], rgb[2] });
    const float high = std::max({ rgb[0], rgb[1], rgb[2] });
    // Match Dark's fixed saturation blend without changing neutral colors or edge coverage.
    for (std::size_t i = 0; i < 3; ++i)
        rgb[i] = (static_cast<float>(foreground[i])
                  + static_cast<float>(background[i] - foreground[i]) * (low + (high - rgb[i]) * 0.4f))
            / 255.0f;
}

void mapVectorColor(fz_context* context,
                    fz_device* device,
                    fz_colorspace*& colorspace,
                    const float*& color,
                    float* rgb,
                    fz_color_params params)
{
    auto* state = reinterpret_cast<ThemeDevice*>(device);
    checkCancellation(context, state);
    // Mask colors encode opacity, not visible color. Recoloring them would
    // change transparency, including that of images we dim.
    if (state->maskDepth || !state->recolorVectors || !colorspace)
        return;
    fz_convert_color(context, colorspace, color, fz_device_rgb(context), rgb, nullptr, params);
    mapColor(rgb, state->background);
    colorspace = fz_device_rgb(context);
    color = rgb;
}

void validateBounds(fz_context* context, fz_irect bounds)
{
    const auto width = std::int64_t(bounds.x1) - bounds.x0;
    const auto height = std::int64_t(bounds.y1) - bounds.y0;
    if (width > Limit::MaxRenderDimension || height > Limit::MaxRenderDimension)
        fz_throw(context, FZ_ERROR_LIMIT, "Theme image, shading or pattern exceeds render dimensions");
}

fz_irect clipPaintBounds(fz_context* context, fz_device* device, fz_irect bounds)
{
    const auto* state = reinterpret_cast<ThemeDevice*>(device);
    bounds = fz_intersect_irect(bounds, state->bounds[state->tileDepth]);
    // Pattern cells use their own bounds; the container scissor describes the repeated area.
    if (!state->tileDepth)
        bounds = fz_intersect_irect(
            bounds, fz_round_rect(fz_transform_rect(fz_device_current_scissor(context, device), state->transform)));
    return bounds;
}

// Classify the whole image independently of zoom, clipping, and requested tiles.
// Weight by alpha so transparent padding is ignored and faint pixels contribute less.
bool isMostlyDarkImage(fz_context* context, const ThemeDevice* state, fz_image* source, fz_color_params params)
{
    fz_pixmap* volatile thumbnail = nullptr;
    fz_device* volatile raster = nullptr;
    bool dark = false;
    fz_var(dark);
    fz_try(context)
    {
        checkCancellation(context, state);
        thumbnail = fz_new_pixmap(context, fz_device_rgb(context), 64, 64, nullptr, 1);
        fz_clear_pixmap(context, thumbnail);
        raster = fz_new_draw_device(context, fz_identity, thumbnail);
        fz_set_default_colorspaces(context, raster, state->defaultColorspaces);
        fz_fill_image(context, raster, source, fz_scale(64, 64), 1, params);
        fz_close_device(context, raster);
        const int stride = fz_pixmap_stride(context, thumbnail);
        const auto* samples = fz_pixmap_samples(context, thumbnail);
        unsigned visibleWeight = 0, darkWeight = 0;
        for (int y = 0; y < 64; ++y) {
            checkCancellation(context, state);
            const auto* pixel = samples + y * stride;
            for (int x = 0; x < 64; ++x, pixel += 4) {
                const unsigned alpha = pixel[3];
                visibleWeight += alpha;
                if (54U * pixel[0] + 183U * pixel[1] + 19U * pixel[2] < 128 * alpha)
                    darkWeight += alpha;
            }
        }
        dark = darkWeight > visibleWeight / 2;
    }
    fz_always(context)
    {
        fz_drop_device(context, raster);
        fz_drop_pixmap(context, thumbnail);
    }
    fz_catch(context) fz_rethrow(context);
    return dark;
}

// Rasterize only the visible image at viewing resolution, leaving source/cache pixels intact.
void fillImage(
    fz_context* context, fz_device* device, fz_image* source, fz_matrix ctm, float alpha, fz_color_params params)
{
    auto* state = reinterpret_cast<ThemeDevice*>(device);
    checkCancellation(context, state);
    if (state->maskDepth) {
        fz_fill_image(context, device->passthrough, source, ctm, alpha, params);
        return;
    }
    const fz_matrix transform = fz_concat(ctm, state->transform);
    const auto bounds = clipPaintBounds(context, device, fz_round_rect(fz_transform_rect(fz_unit_rect, transform)));
    if (fz_is_empty_irect(bounds) || alpha == 0)
        return;
    validateBounds(context, bounds);
    const bool invertImage = state->theme == Model::ReadingTheme::Dark && state->recolorVectors
        && !isMostlyDarkImage(context, state, source, params);
    fz_pixmap* volatile pixmap = nullptr;
    fz_device* volatile raster = nullptr;
    fz_image* volatile image = nullptr;
    fz_try(context)
    {
        pixmap = fz_new_pixmap_with_bbox(context, fz_device_rgb(context), bounds, nullptr, 1);
        fz_clear_pixmap(context, pixmap);
        raster = fz_new_draw_device(context, fz_identity, pixmap);
        fz_enable_device_hints(context, raster, device->passthrough->hints & FZ_DONT_INTERPOLATE_IMAGES);
        fz_set_default_colorspaces(context, raster, state->defaultColorspaces);
        fz_fill_image(context, raster, source, transform, 1, params);
        fz_close_device(context, raster);
        const int width = fz_pixmap_width(context, pixmap);
        const int height = fz_pixmap_height(context, pixmap);
        const int stride = fz_pixmap_stride(context, pixmap);
        auto* samples = fz_pixmap_samples(context, pixmap);
        if (invertImage)
            applyDocumentTheme(context, pixmap, state->theme, state->cookie);
        else
            for (int y = 0; y < height; ++y) {
                checkCancellation(context, state);
                auto* pixel = samples + std::size_t(y) * std::size_t(stride);
                dimImageRow(pixel, width);
            }
        image = fz_new_image_from_pixmap(context, pixmap, nullptr);
        const fz_matrix position { float(width), 0, 0, float(height), float(bounds.x0), float(bounds.y0) };
        fz_fill_image(context,
                      device->passthrough,
                      image,
                      fz_concat(position, fz_invert_matrix(state->transform)),
                      alpha,
                      params);
    }
    fz_always(context)
    {
        fz_drop_image(context, image);
        fz_drop_device(context, raster);
        fz_drop_pixmap(context, pixmap);
    }
    fz_catch(context) fz_rethrow(context);
}

void fillShade(
    fz_context* context, fz_device* device, fz_shade* shade, fz_matrix ctm, float alpha, fz_color_params params)
{
    auto* state = reinterpret_cast<ThemeDevice*>(device);
    checkCancellation(context, state);
    if (state->maskDepth || !state->recolorVectors) {
        fz_fill_shade(context, device->passthrough, shade, ctm, alpha, params);
        return;
    }
    // Shade color tables and meshes belong to the source document. Rasterize
    // just this mark, recolor its unpremultiplied RGB, then composite it at the
    // original position in the drawing sequence and clipping/group stack.
    const fz_matrix transform = fz_concat(ctm, state->transform);
    const auto bounds = clipPaintBounds(context, device, fz_round_rect(fz_bound_shade(context, shade, transform)));
    if (fz_is_empty_irect(bounds) || alpha == 0)
        return;
    validateBounds(context, bounds);
    fz_pixmap* volatile pixmap = nullptr;
    fz_device* volatile raster = nullptr;
    fz_image* volatile image = nullptr;
    fz_try(context)
    {
        pixmap = fz_new_pixmap_with_bbox(context, fz_device_rgb(context), bounds, nullptr, 1);
        fz_clear_pixmap(context, pixmap);
        raster = fz_new_draw_device(context, fz_identity, pixmap);
        fz_fill_shade(context, raster, shade, transform, 1, params);
        fz_close_device(context, raster);
        const int width = fz_pixmap_width(context, pixmap);
        const int height = fz_pixmap_height(context, pixmap);
        const int stride = fz_pixmap_stride(context, pixmap);
        auto* samples = fz_pixmap_samples(context, pixmap);
        for (int y = 0; y < height; ++y) {
            checkCancellation(context, state);
            auto* pixel = samples + std::size_t(y) * std::size_t(stride);
            for (int x = 0; x < width; ++x, pixel += 4) {
                if (!pixel[3])
                    continue;
                float rgb[3] { float(pixel[0]) / pixel[3], float(pixel[1]) / pixel[3], float(pixel[2]) / pixel[3] };
                mapColor(rgb, state->background);
                for (int i = 0; i < 3; ++i)
                    pixel[i] = static_cast<unsigned char>(std::clamp(rgb[i] * pixel[3] + 0.5f, 0.0f, float(pixel[3])));
            }
        }
        image = fz_new_image_from_pixmap(context, pixmap, nullptr);
        const fz_matrix position { float(width), 0, 0, float(height), float(bounds.x0), float(bounds.y0) };
        fz_fill_image(context,
                      device->passthrough,
                      image,
                      fz_concat(position, fz_invert_matrix(state->transform)),
                      alpha,
                      params);
    }
    fz_always(context)
    {
        fz_drop_image(context, image);
        fz_drop_device(context, raster);
        fz_drop_pixmap(context, pixmap);
    }
    fz_catch(context) fz_rethrow(context);
}

} // namespace

fz_device* newThemeDevice(fz_context* context,
                          fz_device* target,
                          fz_matrix transform,
                          fz_irect bounds,
                          const fz_cookie* cookie,
                          Model::ReadingTheme theme,
                          bool recolorVectors)
{
    if (theme != Model::ReadingTheme::Dusk && theme != Model::ReadingTheme::Dark)
        fz_throw(context, FZ_ERROR_ARGUMENT, "theme device requires Dusk or Dark");
    auto* state =
        reinterpret_cast<ThemeDevice*>(fz_new_passthrough_device_of_size(context, target, sizeof(ThemeDevice)));
    state->transform = transform;
    state->cookie = cookie;
    state->theme = theme;
    state->background = Model::ThemeColor::splitRgb(Model::ThemeColor::getBackground(theme));
    state->recolorVectors = recolorVectors;
    state->bounds[0] = bounds;
    auto* device = &state->super;
    device->fill_path = [](fz_context* ctx,
                           fz_device* dev,
                           const fz_path* path,
                           int evenOdd,
                           fz_matrix ctm,
                           fz_colorspace* cs,
                           const float* color,
                           float alpha,
                           fz_color_params params) {
        float rgb[3];
        mapVectorColor(ctx, dev, cs, color, rgb, params);
        fz_fill_path(ctx, dev->passthrough, path, evenOdd, ctm, cs, color, alpha, params);
    };
    device->stroke_path = [](fz_context* ctx,
                             fz_device* dev,
                             const fz_path* path,
                             const fz_stroke_state* stroke,
                             fz_matrix ctm,
                             fz_colorspace* cs,
                             const float* color,
                             float alpha,
                             fz_color_params params) {
        float rgb[3];
        mapVectorColor(ctx, dev, cs, color, rgb, params);
        fz_stroke_path(ctx, dev->passthrough, path, stroke, ctm, cs, color, alpha, params);
    };
    device->fill_text = [](fz_context* ctx,
                           fz_device* dev,
                           const fz_text* text,
                           fz_matrix ctm,
                           fz_colorspace* cs,
                           const float* color,
                           float alpha,
                           fz_color_params params) {
        float rgb[3];
        mapVectorColor(ctx, dev, cs, color, rgb, params);
        fz_fill_text(ctx, dev->passthrough, text, ctm, cs, color, alpha, params);
    };
    device->stroke_text = [](fz_context* ctx,
                             fz_device* dev,
                             const fz_text* text,
                             const fz_stroke_state* stroke,
                             fz_matrix ctm,
                             fz_colorspace* cs,
                             const float* color,
                             float alpha,
                             fz_color_params params) {
        float rgb[3];
        mapVectorColor(ctx, dev, cs, color, rgb, params);
        fz_stroke_text(ctx, dev->passthrough, text, stroke, ctm, cs, color, alpha, params);
    };
    device->fill_shade = fillShade;
    device->fill_image = fillImage;
    device->set_default_colorspaces = [](fz_context* ctx, fz_device* dev, fz_default_colorspaces* spaces) {
        auto* state = reinterpret_cast<ThemeDevice*>(dev);
        auto* kept = fz_keep_default_colorspaces(ctx, spaces);
        fz_drop_default_colorspaces(ctx, state->defaultColorspaces);
        state->defaultColorspaces = kept;
        fz_set_default_colorspaces(ctx, dev->passthrough, spaces);
    };
    // Stencil images carry coverage, not colors (including bitmap font glyphs).
    // Theme their paint color like a vector mark while preserving the mask.
    device->fill_image_mask = [](fz_context* ctx,
                                 fz_device* dev,
                                 fz_image* image,
                                 fz_matrix ctm,
                                 fz_colorspace* cs,
                                 const float* color,
                                 float alpha,
                                 fz_color_params params) {
        float rgb[3];
        mapVectorColor(ctx, dev, cs, color, rgb, params);
        fz_fill_image_mask(ctx, dev->passthrough, image, ctm, cs, color, alpha, params);
    };
    device->begin_mask = [](fz_context* ctx,
                            fz_device* dev,
                            fz_rect area,
                            int luminosity,
                            fz_colorspace* cs,
                            const float* background,
                            fz_color_params params) {
        ++reinterpret_cast<ThemeDevice*>(dev)->maskDepth;
        fz_begin_mask(ctx, dev->passthrough, area, luminosity, cs, background, params);
    };
    device->end_mask = [](fz_context* ctx, fz_device* dev, fz_function* function) {
        fz_end_mask_tr(ctx, dev->passthrough, function);
        --reinterpret_cast<ThemeDevice*>(dev)->maskDepth;
    };
    device->begin_tile = [](fz_context* ctx,
                            fz_device* dev,
                            fz_rect area,
                            fz_rect view,
                            float xstep,
                            float ystep,
                            fz_matrix ctm,
                            int,
                            int docId) {
        auto* tile = reinterpret_cast<ThemeDevice*>(dev);
        if (tile->tileDepth == Limit::MaxDepth)
            fz_throw(ctx, FZ_ERROR_LIMIT, "Theme pattern nesting exceeds limit");
        const auto bounds = fz_round_rect(fz_transform_rect(view, fz_concat(ctm, tile->transform)));
        validateBounds(ctx, bounds);
        tile->bounds[++tile->tileDepth] = bounds;
        // A pattern cached by another theme must not be reused here, nor may
        // this recolored pattern enter the cache used by normal rendering.
        return fz_begin_tile_tid(ctx, dev->passthrough, area, view, xstep, ystep, ctm, 0, docId);
    };
    device->end_tile = [](fz_context* ctx, fz_device* dev) {
        fz_end_tile(ctx, dev->passthrough);
        --reinterpret_cast<ThemeDevice*>(dev)->tileDepth;
    };
    device->drop_device = [](fz_context* ctx, fz_device* dev) {
        auto* state = reinterpret_cast<ThemeDevice*>(dev);
        if (state->cookie && state->cookie->abort)
            dev->passthrough->close_device = nullptr;
        fz_drop_default_colorspaces(ctx, state->defaultColorspaces);
        fz_drop_device(ctx, dev->passthrough);
    };
    return device;
}

} // namespace Mu::Worker::Engine
