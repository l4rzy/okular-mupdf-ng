// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_MODEL_VALIDATION_HPP
#define MU_SHARED_MODEL_VALIDATION_HPP

#include <cstddef>
#include <string_view>

#include "shared/model/types.hpp"

namespace Mu::Model {

/// Validates page dimensions against the regular or tiled render limit.
/// Both dimensions must be positive and within the selected limit.
bool isValidRenderDimensions(int width, int height, bool tiled) noexcept;

/// Validates that a tile is a positive, in-bounds rectangle within an image.
bool isValidRenderTile(int imageWidth, int imageHeight, int tileX, int tileY, int tileWidth, int tileHeight) noexcept;

/// Validates finite, positive horizontal and vertical render resolution.
bool isValidDpi(double dpiX, double dpiY) noexcept;

/// Validates OCR resolution against the supported 72–600 DPI range.
bool isValidOcrDpi(float dpi) noexcept;

/// Validates a descriptor transfer identifier before it reaches the FD channel.
bool isValidFileTransfer(const FileTransfer& transfer, std::string_view* reason = nullptr) noexcept;

/// Validates request-level render geometry. Document and page-count state stay
/// in the command boundary because they are not available to the plugin.
bool isValidRenderRequest(const RenderRequest& request, std::string_view* reason = nullptr) noexcept;

/// Validates request-level text extraction geometry.
bool isValidTextBoxesRequest(const TextBoxesRequest& request, std::string_view* reason = nullptr) noexcept;

/// Validates request-level OCR inputs. Empty language means the worker default.
bool isValidOcrPageRequest(const OcrPageRequest& request, std::string_view* reason = nullptr) noexcept;

/// Validates worker rendering and EPUB layout settings before they are applied.
bool isValidDocumentSettings(const DocumentSettings& settings, std::string_view* reason = nullptr) noexcept;

namespace Detail {

/// Recursively validates extension values while enforcing depth and entry limits.
bool isValidAnnotationExtensionValue(const Value& value, std::size_t depth, std::size_t& entries);

/// Validates extension keys and all values in an annotation extension object.
bool isValidAnnotationExtension(const Value::Object& extension);

} // namespace Detail

/// Reports whether a string contains only well-formed Unicode UTF-8 sequences.
bool isValidUtf8(std::string_view str) noexcept;

/// Counts non-continuation bytes, which represent code points in valid UTF-8.
std::size_t utf8CodepointCount(std::string_view str) noexcept;

/// Validates standard base64 syntax: 4-byte alignment, the standard alphabet
/// (A-Z, a-z, 0-9, +, /), and '=' padding only in the last two positions
/// (padding itself is optional). Empty input is valid.
bool isValidBase64(std::string_view encoded) noexcept;

/// Validates stored EPUB custom CSS in its encoded form: standard base64
/// syntax plus the stored byte cap. Empty input is valid and means no CSS.
bool isValidEpubCustomCssBase64(std::string_view encoded) noexcept;

/// Reports whether a string is safe to pass to NUL-terminated APIs.
bool hasNoEmbeddedNul(std::string_view str) noexcept;

/// Validates a rectangle expressed as finite coordinates normalized to [0, 1].
bool isValidNormalizedRect(const NormalizedRect& rect) noexcept;

/// Validates annotation strings, geometry collections, and extension data.
/// On failure, optionally stores a stable diagnostic in `reason`.
bool isValidAnnotation(const Annotation& annotation, std::string_view* reason = nullptr);

/// Validates a form field's identity, geometry, text, choices, and selections.
/// On failure, optionally stores a stable diagnostic in `reason`.
bool isValidFormField(const FormField& field, std::string_view* reason = nullptr);

/// Validates one form value and its type-specific limits.
/// On failure, optionally stores a stable diagnostic in `reason`.
bool isValidFormValue(const FormValue& value, std::string_view* reason = nullptr);

/// Validates a form update handle and the value assigned to it.
/// On failure, optionally stores a stable diagnostic in `reason`.
bool isValidFormUpdateRequest(const FormUpdateRequest& request, std::string_view* reason = nullptr);

/// Validates the handle carried by a form reset request.
/// On failure, optionally stores a stable diagnostic in `reason`.
bool isValidFormResetRequest(const FormResetRequest& request, std::string_view* reason = nullptr);

/// Validates an annotation add request before document state is consulted.
bool isValidAnnotationAddRequest(const AnnotationAddRequest& request, std::string_view* reason = nullptr);

/// Validates an annotation modification request before handle lookup.
bool isValidAnnotationModifyRequest(const AnnotationModifyRequest& request, std::string_view* reason = nullptr);

/// Validates an annotation removal request before handle lookup.
bool isValidAnnotationRemoveRequest(const AnnotationRemoveRequest& request, std::string_view* reason = nullptr);

/// Validates signing fields independent of the open document and backend.
bool isValidSignRequest(const SignRequest& request, std::string_view* reason = nullptr);

} // namespace Mu::Model

#endif // MU_SHARED_MODEL_VALIDATION_HPP
