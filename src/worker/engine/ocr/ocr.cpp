// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ocr/ocr.hpp"
#include "engine/constants.hpp"
#include "engine/mupdf_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
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

#ifdef MUPDF_HAS_OCR
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
    ImagePresenceDevice* detector = nullptr;
    bool hasImage = false;
    fz_var(detector);
    fz_var(hasImage);
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
#endif

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

// =============================================================================
// Synchronous OCR Page Processing
// =============================================================================

// Runs in a private fz_context so a hostile page cannot corrupt the shared
// engine document. inputFd is adopted (fdopen) and closed on every path.
// Cancellation sets cookie.abort mid-render and is re-checked per character;
// boxes are normalized to [0,1], clamping non-finite and surrogate/out-of-range
// codepoints.
::Mu::Model::OcrResult runOcr(int inputFd,
                              const std::string& password,
                              int pageNumber,
                              const std::string& language,
                              float dpi,
                              CancellationCookie* cookie)
{
    ::Mu::Model::OcrResult result;
    Mu::Worker::Sys::FileDescriptor fd(inputFd);
    if (!fd || pageNumber < 0 || !isValidOcrDpi(dpi)) {
        result.status = ::Mu::Model::OcrStatus::Failed;
        return result;
    }
#ifndef MUPDF_HAS_OCR
    (void)password;
    (void)language;
    (void)cookie;
    result.status = ::Mu::Model::OcrStatus::Unavailable;
    return result;
#else
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
    fz_stream* stream = nullptr;
    fz_document* document = nullptr;
    fz_page* page = nullptr;
    fz_stext_page* text = nullptr;
    fz_device* textDevice = nullptr;
    fz_device* ocrDevice = nullptr;
    fz_var(stream);
    fz_var(document);
    fz_var(page);
    fz_var(text);
    fz_var(textDevice);
    fz_var(ocrDevice);
    CancellationCookie fallbackCookie;
    CancellationCookie* activeCookie = cookie ? cookie : &fallbackCookie;
    bool failed = false;
    fz_var(failed);
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
            // Step 3: Instantiate Tesseract OCR engine filter device
            text = fz_new_stext_page(context, fz_empty_rect);
            textDevice = fz_new_stext_device(context, text, nullptr);
#ifdef TESSDATA_DIR
            ocrDevice = fz_new_ocr_device(context,
                                          textDevice,
                                          fz_scale(dpi / 72.0f, dpi / 72.0f),
                                          { 0, 0, width, height },
                                          1,
                                          lang.c_str(),
                                          TESSDATA_DIR,
                                          nullptr,
                                          nullptr);
#else
            ocrDevice = fz_new_ocr_device(context,
                                          textDevice,
                                          fz_scale(dpi / 72.0f, dpi / 72.0f),
                                          { 0, 0, width, height },
                                          1,
                                          lang.c_str(),
                                          nullptr,
                                          nullptr,
                                          nullptr);
#endif
            activeCookie->sync();
            if (activeCookie->isCancelled()) {
                fz_throw(context, FZ_ERROR_ABORT, "OCR job cancelled before render");
            }

            // Step 4: Run page through OCR device to perform text recognition
            fz_run_page(context, page, ocrDevice, fz_scale(dpi / 72.0f, dpi / 72.0f), activeCookie->get());
            fz_close_device(context, ocrDevice);
            fz_close_device(context, textDevice);

            if (activeCookie->isCancelled() || activeCookie->get()->abort) {
                result.status = ::Mu::Model::OcrStatus::Cancelled;
            } else if (activeCookie->get()->errors) {
                failed = true;
            } else {
                // Step 5: Extract recognized text characters and normalize coordinate quads
                const double scale = dpi / 72.0;
                const double originX = bounds.x0 * scale;
                const double originY = bounds.y0 * scale;

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
                            const auto& quad = character->quad;
                            const double left =
                                (std::min({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x }) - originX) / (width * scale);
                            const double top =
                                (std::min({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y }) - originY) / (height * scale);
                            const double right =
                                (std::max({ quad.ul.x, quad.ur.x, quad.ll.x, quad.lr.x }) - originX) / (width * scale);
                            const double bottom =
                                (std::max({ quad.ul.y, quad.ur.y, quad.ll.y, quad.lr.y }) - originY) / (height * scale);
                            if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right)
                                || !std::isfinite(bottom))
                                continue;
                            if (character->c < 0 || character->c > 0x10ffff
                                || (character->c >= 0xd800 && character->c <= 0xdfff))
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
        if (ocrDevice)
            fz_drop_device(context, ocrDevice);
        if (textDevice)
            fz_drop_device(context, textDevice);
        if (text)
            fz_drop_stext_page(context, text);
        if (page)
            fz_drop_page(context, page);
        if (document)
            fz_drop_document(context, document);
        if (stream)
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
#endif
}

} // namespace Mu::Worker::Engine
