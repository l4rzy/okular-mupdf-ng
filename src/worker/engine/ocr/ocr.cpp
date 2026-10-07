// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ocr/ocr.hpp"
#include "engine/constants.hpp"
#include "engine/mupdf_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

#include "shared/model/types.hpp"
#include "shared/model/validation.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Lightweight Image Presence Detector Device
// =============================================================================

namespace {

struct ImagePresenceDevice {
    fz_device super;
    bool hasImage = false;
};

void detectImage(fz_context*, fz_device* device, fz_image*, fz_matrix, float, fz_color_params)
{
    static_cast<ImagePresenceDevice*>(static_cast<void*>(device))->hasImage = true;
}

void detectImageMask(
    fz_context*, fz_device* device, fz_image*, fz_matrix, fz_colorspace*, const float*, float, fz_color_params)
{
    static_cast<ImagePresenceDevice*>(static_cast<void*>(device))->hasImage = true;
}

/// Runs a fast dummy device pass over the page to determine if any image elements exist.
/// If a page contains only vector paths and text, expensive Tesseract OCR can be skipped.
bool pageHasImages(fz_context* context, fz_page* page, fz_cookie* cookie)
{
    ImagePresenceDevice* volatile detector = nullptr;
    volatile bool hasImage = false;
    fz_try(context)
    {
        detector = reinterpret_cast<ImagePresenceDevice*>(fz_new_device_of_size(context, sizeof(ImagePresenceDevice)));
        detector->hasImage = false;
        detector->super.fill_image = detectImage;
        detector->super.fill_image_mask = detectImageMask;
        fz_run_page(context, page, &detector->super, fz_identity, cookie);
        hasImage = detector->hasImage;
    }
    fz_always(context)
    {
        if (detector)
            fz_drop_device(context, &detector->super);
    }
    fz_catch(context)
    {
        fz_rethrow(context);
    }
    return hasImage;
}

// Preserve image painting and its clipping/compositing. Paint other marks white
// to retain their occlusion without recognizing vector text; soft masks keep color.
struct ImageOcrDevice {
    fz_device super;
    int maskDepth = 0;
};

void maskVectorColor(fz_context* context, fz_device* device, fz_colorspace*& colorspace, const float*& color)
{
    static constexpr float white = 1;
    if (reinterpret_cast<ImageOcrDevice*>(device)->maskDepth <= 0) {
        colorspace = fz_device_gray(context);
        color = &white;
    }
}

fz_device* newImageOcrDevice(fz_context* context, fz_device* target)
{
    auto* device =
        reinterpret_cast<ImageOcrDevice*>(fz_new_passthrough_device_of_size(context, target, sizeof(ImageOcrDevice)));
    device->super.fill_path = [](fz_context* ctx,
                                 fz_device* dev,
                                 const fz_path* path,
                                 int evenOdd,
                                 fz_matrix ctm,
                                 fz_colorspace* cs,
                                 const float* color,
                                 float alpha,
                                 fz_color_params params) {
        maskVectorColor(ctx, dev, cs, color);
        fz_fill_path(ctx, dev->passthrough, path, evenOdd, ctm, cs, color, alpha, params);
    };
    device->super.stroke_path = [](fz_context* ctx,
                                   fz_device* dev,
                                   const fz_path* path,
                                   const fz_stroke_state* stroke,
                                   fz_matrix ctm,
                                   fz_colorspace* cs,
                                   const float* color,
                                   float alpha,
                                   fz_color_params params) {
        maskVectorColor(ctx, dev, cs, color);
        fz_stroke_path(ctx, dev->passthrough, path, stroke, ctm, cs, color, alpha, params);
    };
    device->super.fill_text = [](fz_context* ctx,
                                 fz_device* dev,
                                 const fz_text* text,
                                 fz_matrix ctm,
                                 fz_colorspace* cs,
                                 const float* color,
                                 float alpha,
                                 fz_color_params params) {
        maskVectorColor(ctx, dev, cs, color);
        fz_fill_text(ctx, dev->passthrough, text, ctm, cs, color, alpha, params);
    };
    device->super.stroke_text = [](fz_context* ctx,
                                   fz_device* dev,
                                   const fz_text* text,
                                   const fz_stroke_state* stroke,
                                   fz_matrix ctm,
                                   fz_colorspace* cs,
                                   const float* color,
                                   float alpha,
                                   fz_color_params params) {
        maskVectorColor(ctx, dev, cs, color);
        fz_stroke_text(ctx, dev->passthrough, text, stroke, ctm, cs, color, alpha, params);
    };
    device->super.ignore_text = nullptr;
    device->super.begin_mask = [](fz_context* ctx,
                                  fz_device* dev,
                                  fz_rect area,
                                  int luminosity,
                                  fz_colorspace* cs,
                                  const float* background,
                                  fz_color_params params) {
        ++reinterpret_cast<ImageOcrDevice*>(dev)->maskDepth;
        fz_begin_mask(ctx, dev->passthrough, area, luminosity, cs, background, params);
    };
    device->super.end_mask = [](fz_context* ctx, fz_device* dev, fz_function* function) {
        fz_end_mask_tr(ctx, dev->passthrough, function);
        --reinterpret_cast<ImageOcrDevice*>(dev)->maskDepth;
    };
    return &device->super;
}

// Native text includes invisible text from an existing OCR layer. Mask the
// actual quads rather than entire image blocks, allowing partially covered scans.
void maskNativeText(fz_context* context,
                    fz_device* device,
                    const fz_stext_page* native,
                    fz_matrix transform,
                    CancellationCookie* cookie)
{
    fz_path* path = fz_new_path(context);
    fz_try(context)
    {
        for (const auto* block = native->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (const auto* line = block->u.t.first_line; line; line = line->next) {
                if (cookie->isCancelled())
                    fz_throw(context, FZ_ERROR_ABORT, "OCR job cancelled while masking native text");
                for (const auto* character = line->first_char; character; character = character->next) {
                    const auto& quad = character->quad;
                    fz_moveto(context, path, quad.ul.x, quad.ul.y);
                    fz_lineto(context, path, quad.ur.x, quad.ur.y);
                    fz_lineto(context, path, quad.lr.x, quad.lr.y);
                    fz_lineto(context, path, quad.ll.x, quad.ll.y);
                    fz_closepath(context, path);
                }
            }
        }
        const float white = 1;
        fz_fill_path(context, device, path, 0, transform, fz_device_gray(context), &white, 1, fz_default_color_params);
    }
    fz_always(context)
    {
        fz_drop_path(context, path);
    }
    fz_catch(context)
    {
        fz_rethrow(context);
    }
}

bool overlapsNativeText(const fz_stext_page* native, fz_quad quad)
{
    const fz_point center { (quad.ul.x + quad.lr.x) / 2, (quad.ul.y + quad.lr.y) / 2 };
    for (const auto* block = native->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT || !fz_is_point_inside_rect(center, block->bbox))
            continue;
        for (const auto* line = block->u.t.first_line; line; line = line->next) {
            if (!fz_is_point_inside_rect(center, line->bbox))
                continue;
            for (const auto* character = line->first_char; character; character = character->next) {
                if (fz_is_point_inside_quad(center, character->quad))
                    return true;
            }
        }
    }
    return false;
}

} // namespace

// =============================================================================
// Language Identifier Sanitization
// =============================================================================

// Okular may pass a per-DPI variant name like "eng_300dpi". Strip both the
// ".traineddata" extension and the trailing "_<digits>dpi" tag so Tesseract
// receives a plain language name, falling back to "eng".
std::string tessdataLanguage(std::string language)
{
    if (language.ends_with(".traineddata"))
        language.resize(language.size() - 12);
    const std::size_t suffix = language.rfind('_');
    if (suffix != std::string::npos && language.ends_with("dpi") && suffix + 4 <= language.size()
        && suffix + 1 < language.size() - 3
        && std::all_of(language.begin() + static_cast<std::ptrdiff_t>(suffix + 1),
                       language.end() - 3,
                       [](unsigned char c) { return c >= '0' && c <= '9'; }))
        language.resize(suffix);
    return language.empty() ? "eng" : language;
}

std::optional<std::string> findTessdataDirectory(const std::string& language,
                                                 const std::vector<std::string>& directories)
{
    const std::string normalized = tessdataLanguage(language);
    std::vector<std::filesystem::path> models;
    for (std::size_t start = 0; start < normalized.size();) {
        const auto end = normalized.find('+', start);
        const std::filesystem::path name(normalized.substr(start, end - start));
        if (name.empty() || name.is_absolute())
            return std::nullopt;
        for (const auto& component : name) {
            if (component == "..")
                return std::nullopt;
        }
        models.emplace_back(name.string() + ".traineddata");
        if (end == std::string::npos)
            break;
        start = end + 1;
        if (start == normalized.size())
            return std::nullopt;
    }
    for (const auto& directory : directories) {
        if (directory.empty())
            continue;
        const bool available = std::all_of(models.begin(), models.end(), [&](const auto& model) {
            std::error_code error;
            return std::filesystem::is_regular_file(std::filesystem::path(directory) / model, error);
        });
        if (available)
            return directory;
    }
    return std::nullopt;
}

// =============================================================================
// Synchronous OCR Page Processing
// =============================================================================

// Runs in a private fz_context so a hostile page cannot corrupt the shared
// engine document. inputFd is adopted (fdopen) and closed on every path.
// Cancellation sets cookie.abort mid-render and is re-checked per character;
// boxes are normalized to [0,1], rejecting non-finite coordinates and invalid
// Unicode codepoints.
::Mu::Model::OcrResult runOcr(int inputFd,
                              const std::string& password,
                              int pageNumber,
                              const std::string& language,
                              float dpi,
                              CancellationCookie* cookie,
                              const std::string& tessDataDirectory)
{
    ::Mu::Model::OcrResult result;
    Mu::Worker::Sys::FileDescriptor fd(inputFd);
    if (!fd || pageNumber < 0 || !isValidOcrDpi(dpi)) {
        result.status = ::Mu::Model::OcrStatus::Failed;
        return result;
    }
    if (cookie && cookie->isCancelled()) {
        result.status = ::Mu::Model::OcrStatus::Cancelled;
        return result;
    }
    fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!context) {
        return result;
    }
    fz_register_document_handlers(context);
    FILE* input = ::fdopen(fd.get(), "rb");
    if (input)
        (void)fd.release();
    fz_stream* volatile stream = nullptr;
    fz_document* volatile document = nullptr;
    fz_page* volatile page = nullptr;
    fz_stext_page* volatile text = nullptr;
    fz_device* volatile textDevice = nullptr;
    fz_device* volatile ocrDevice = nullptr;
    fz_device* volatile imageDevice = nullptr;
    fz_device* volatile nativeDevice = nullptr;
    fz_stext_page* volatile nativeText = nullptr;
    CancellationCookie fallbackCookie;
    CancellationCookie* activeCookie = cookie ? cookie : &fallbackCookie;
    volatile bool failed = false;
    const std::string lang = tessdataLanguage(language);

    fz_try(context)
    {
        if (!input)
            fz_throw(context, FZ_ERROR_GENERIC, "could not adopt OCR input FD");

        // Step 1: Open document stream and load target page
        stream = fz_open_file_ptr_no_close(context, input);
        document = fz_open_document_with_stream(context, "pdf", stream);
        if (!password.empty() && !fz_authenticate_password(context, document, password.c_str()))
            fz_throw(context, FZ_ERROR_GENERIC, "incorrect password");
        page = fz_load_page(context, document, pageNumber);
        const fz_rect bounds = fz_bound_page(context, page);
        const float width = bounds.x1 - bounds.x0;
        const float height = bounds.y1 - bounds.y0;
        if (!(width > 0 && height > 0))
            fz_throw(context, FZ_ERROR_GENERIC, "page has invalid bounds");

        activeCookie->sync();

        // Step 2: Check if page contains images to OCR
        if (!pageHasImages(context, page, activeCookie->get())) {
            result.status =
                activeCookie->isCancelled() ? ::Mu::Model::OcrStatus::Cancelled : ::Mu::Model::OcrStatus::Success;
        } else {
            nativeText = fz_new_stext_page(context, bounds);
            fz_stext_options options { };
            options.flags = FZ_STEXT_CLIP;
            nativeDevice = fz_new_stext_device(context, nativeText, &options);
            fz_run_page_contents(context, page, nativeDevice, fz_identity, activeCookie->get());
            fz_close_device(context, nativeDevice);
            if (countStextChars(nativeText) > Constant::MaxOcrBoxes)
                fz_throw(context, FZ_ERROR_LIMIT, "too many native text boxes for image OCR");
            const float renderScale = static_cast<float>(dpi / Constant::PointsPerInch);
            const fz_matrix scale = fz_scale(renderScale, renderScale);
            // Recognize the displayed orientation, including the PDF's page rotation.
            const fz_matrix transform = fz_concat(fz_translate(-bounds.x0, -bounds.y0), scale);
            const fz_rect canvas { 0, 0, width, height };
            const fz_matrix inverse = fz_invert_matrix(transform);
            // Step 3: Instantiate Tesseract OCR engine filter device
            text = fz_new_stext_page(context, fz_empty_rect);
            textDevice = fz_new_stext_device(context, text, nullptr);
            ocrDevice = fz_new_ocr_device(
                context,
                textDevice,
                scale,
                canvas,
                0,
                lang.c_str(),
                tessDataDirectory.empty() ? TESSDATA_DIR : tessDataDirectory.c_str(),
                [](fz_context*, void* opaque, int) {
                    return static_cast<CancellationCookie*>(opaque)->isCancelled() ? 1 : 0;
                },
                activeCookie);
            activeCookie->sync();
            if (activeCookie->isCancelled()) {
                fz_throw(context, FZ_ERROR_ABORT, "OCR job cancelled before render");
            }

            // Step 4: Run page through OCR device to perform text recognition
            imageDevice = newImageOcrDevice(context, ocrDevice);
            fz_run_page_contents(context, page, imageDevice, transform, activeCookie->get());
            maskNativeText(context, ocrDevice, nativeText, transform, activeCookie);
            // Closing the forwarding device also closes its OCR target.
            fz_close_device(context, imageDevice);
            fz_close_device(context, textDevice);

            if (activeCookie->isCancelled() || activeCookie->get()->abort) {
                result.status = ::Mu::Model::OcrStatus::Cancelled;
            } else if (activeCookie->get()->errors) {
                failed = true;
            } else {
                // Step 5: Extract recognized text characters and normalize coordinate quads
                const std::size_t charCount = countStextChars(text);
                if (charCount > 0)
                    result.boxes.reserve(std::min(charCount, Constant::MaxOcrBoxes));

                for (fz_stext_block* block = text->first_block; block; block = block->next) {
                    if (block->type != FZ_STEXT_BLOCK_TEXT)
                        continue;
                    for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                        for (fz_stext_char* character = line->first_char; character; character = character->next) {
                            if (activeCookie->isCancelled())
                                break;
                            if (result.boxes.size() >= Constant::MaxOcrBoxes) {
                                failed = true;
                                break;
                            }
                            // MuPDF's no-list OCR mode uses Courier at 10/6 of the
                            // word height. Restore list mode's vertical scale around
                            // the baseline so selection boxes do not span adjacent lines.
                            constexpr float heightScale = 6.0f / 10.0f;
                            const fz_matrix correction { 1,           0, 0,
                                                         heightScale, 0, character->origin.y * (1 - heightScale) };
                            const auto quad = fz_transform_quad(character->quad, fz_concat(correction, inverse));
                            if (overlapsNativeText(nativeText, quad))
                                continue;
                            const double left =
                                (std::min({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x }) - bounds.x0) / width;
                            const double top =
                                (std::min({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y }) - bounds.y0) / height;
                            const double right =
                                (std::max({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x }) - bounds.x0) / width;
                            const double bottom =
                                (std::max({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y }) - bounds.y0) / height;
                            if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right)
                                || !std::isfinite(bottom))
                                continue;
                            if (character->c < 0 || character->c > Constant::UnicodeMaxCodePoint
                                || (character->c >= Constant::UnicodeSurrogateMin
                                    && character->c <= Constant::UnicodeSurrogateMax))
                                continue;
                            char utf8[4] { };
                            const int length = fz_runetochar(utf8, character->c);
                            if (length <= 0)
                                continue;
                            result.boxes.emplace_back(std::string(utf8, static_cast<std::size_t>(length)),
                                                      std::clamp(left, 0.0, 1.0),
                                                      std::clamp(top, 0.0, 1.0),
                                                      std::clamp(right, 0.0, 1.0),
                                                      std::clamp(bottom, 0.0, 1.0),
                                                      false);
                        }
                        if (failed || activeCookie->isCancelled())
                            break;
                    }
                    if (failed || activeCookie->isCancelled())
                        break;
                }
                result.status = activeCookie->isCancelled() ? ::Mu::Model::OcrStatus::Cancelled
                    : failed                                ? ::Mu::Model::OcrStatus::Failed
                                                            : ::Mu::Model::OcrStatus::Success;
            }
        }
    }
    fz_always(context)
    {
        fz_drop_device(context, imageDevice);
        fz_drop_device(context, nativeDevice);
        fz_drop_stext_page(context, nativeText);
        fz_drop_device(context, ocrDevice);
        fz_drop_device(context, textDevice);
        fz_drop_stext_page(context, text);
        fz_drop_page(context, page);
        fz_drop_document(context, document);
        fz_drop_stream(context, stream);
    }
    fz_catch(context)
    {
        // MuPDF, input, and OCR-device failures all map to Failed here; the
        // public result has no finer-grained transport error category.
        failed = true;
    }
    if (input)
        ::fclose(input);
    fz_drop_context(context);
    if (failed)
        result.status = ::Mu::Model::OcrStatus::Failed;
    return result;
}

} // namespace Mu::Worker::Engine
