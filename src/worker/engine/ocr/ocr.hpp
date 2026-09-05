// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_OCR_OCR_HPP
#define MU_WORKER_ENGINE_OCR_OCR_HPP

#include <string>

#include "engine/cancellation_cookie.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

/// Extracts clean Tesseract language identifier from traineddata filename or path.
/// Strips `.traineddata` file extension and trailing DPI suffixes like `_300dpi`.
std::string tessdataLanguage(std::string language);

/**
 * Runs Tesseract OCR text recognition on a document page.
 *
 * Execution Steps:
 * 1. Opens source document in an isolated Fitz context (`fz_context`).
 * 2. Probes page for bitmap image presence (`pageHasImages`).
 * 3. Renders page using `fz_new_ocr_device` backed by Tesseract traineddata.
 * 4. Traverses recognized structured text blocks and emits normalized [0, 1] character boxes.
 * 5. Respects cooperative cancellation via `CancellationCookie`.
 *
 * @param inputFd Duplicated file descriptor of the document file.
 * @param password Optional document unlock password.
 * @param pageNumber 0-indexed page number to OCR.
 * @param language Tesseract language identifier (e.g. "eng", "vie").
 * @param dpi Target rendering rasterization resolution.
 * @param cookie Optional cancellation cookie checked periodically during layout and OCR.
 * @return OcrResult with status and extracted character bounding boxes.
 */
::Mu::Model::OcrResult runOcr(int inputFd,
                              const std::string& password,
                              int pageNumber,
                              const std::string& language,
                              float dpi,
                              CancellationCookie* cookie = nullptr);

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_OCR_OCR_HPP
