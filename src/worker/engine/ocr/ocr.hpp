// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_OCR_OCR_HPP
#define MU_WORKER_ENGINE_OCR_OCR_HPP

#include <optional>
#include <string>
#include <vector>

#include "engine/cancellation_cookie.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

/// Extracts clean Tesseract language identifier from traineddata filename or path.
/// Strips `.traineddata` file extension and trailing DPI suffixes like `_300dpi`.
std::string tessdataLanguage(std::string language);

/// Finds the first configured root containing every requested language model.
/// Model names may include relative subdirectories, but cannot contain parent traversal.
std::optional<std::string> findTessdataDirectory(const std::string& language,
                                                 const std::vector<std::string>& directories);

/**
 * Recognizes image text not already covered by a document page's text layer.
 *
 * Uses an isolated Fitz context and Tesseract, preserving image occlusion and
 * masking existing native/hidden text. Supports cooperative cancellation.
 *
 * @param inputFd Duplicated file descriptor of the document file.
 * @param password Optional document unlock password.
 * @param pageNumber 0-indexed page number to OCR.
 * @param language Tesseract language identifier (e.g. "eng", "vie").
 * @param dpi Target rendering rasterization resolution.
 * @param cookie Optional cancellation cookie checked periodically during layout and OCR.
 * @param tessDataDirectory Selected model directory; empty uses the compiled default.
 * @return OcrResult with status and normalized character boxes to add to native text.
 */
::Mu::Model::OcrResult runOcr(int inputFd,
                              const std::string& password,
                              int pageNumber,
                              const std::string& language,
                              float dpi,
                              CancellationCookie* cookie = nullptr,
                              const std::string& tessDataDirectory = { });

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_OCR_OCR_HPP
