// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/model/validation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "shared/protocol/limits.hpp"

namespace Mu::Model {

bool isValidRenderDimensions(int width, int height, bool tiled) noexcept
{
    // Tiled renders may use a larger working size than a single-page render,
    // but both paths still reject zero, negative, and oversized dimensions.
    const int maximum = tiled ? Limit::MaxTiledRenderDimension : Limit::MaxRenderDimension;
    return width > 0 && height > 0 && width <= maximum && height <= maximum;
}

bool isValidRenderTile(int imageWidth, int imageHeight, int tileX, int tileY, int tileWidth, int tileHeight) noexcept
{
    // Check the origin before subtracting so the extent comparisons remain
    // meaningful and cannot accept a tile that crosses an image edge.
    return tileX >= 0 && tileY >= 0 && tileWidth > 0 && tileHeight > 0 && tileX <= imageWidth && tileY <= imageHeight
        && tileWidth <= imageWidth - tileX && tileHeight <= imageHeight - tileY;
}

bool isValidDpi(double dpiX, double dpiY) noexcept
{
    // isfinite also excludes NaN and infinities, which would otherwise bypass
    // ordinary range comparisons.
    return std::isfinite(dpiX) && std::isfinite(dpiY) && dpiX > 0.0 && dpiY > 0.0;
}

bool isValidOcrDpi(float dpi) noexcept
{
    // OCR is intentionally narrower than general rendering because the
    // recognition backend supports a bounded practical resolution range.
    return std::isfinite(dpi) && dpi >= 72.0f && dpi <= 600.0f;
}

namespace Detail {

bool isValidAnnotationExtensionValue(const Value& value, std::size_t depth, std::size_t& entries)
{
    // Extensions are recursive user data. Bound both nesting and aggregate
    // values before descending so malformed input cannot cause unbounded work.
    if (depth > Limit::MaxAnnotationExtensionDepth || ++entries > Limit::MaxAnnotationExtensionEntries)
        return false;
    if (const auto* number = std::get_if<double>(&value.data)) {
        // JSON-like numeric values must not carry NaN or infinity into callers.
        return std::isfinite(*number);
    }
    if (const auto* array = std::get_if<Value::Array>(&value.data)) {
        for (const Value& item : *array)
            if (!isValidAnnotationExtensionValue(item, depth + 1, entries))
                return false;
    }
    if (const auto* object = std::get_if<Value::Object>(&value.data)) {
        // Empty keys are rejected because extension objects use keys as their
        // stable field names when they cross the protocol boundary.
        for (const auto& [key, item] : *object)
            if (key.empty() || !isValidAnnotationExtensionValue(item, depth + 1, entries))
                return false;
    }
    return true;
}

bool isValidAnnotationExtension(const Value::Object& extension)
{
    // Count the root object as part of the extension budget, then account for
    // every nested value while walking the object tree.
    std::size_t entries = 1;
    for (const auto& [key, value] : extension)
        if (key.empty() || !isValidAnnotationExtensionValue(value, 1, entries))
            return false;
    return true;
}

} // namespace Detail

bool isValidUtf8(std::string_view str) noexcept
{
    // Validate one complete UTF-8 sequence at a time. The boundary checks also
    // reject overlong encodings, UTF-16 surrogate halves, and code points above
    // U+10FFFF.
    const auto* s = reinterpret_cast<const std::uint8_t*>(str.data());
    std::size_t i = 0;
    const std::size_t len = str.size();
    while (i < len) {
        if (s[i] <= 0x7F) {
            ++i;
        } else if ((s[i] & 0xE0) == 0xC0) {
            if (i + 1 >= len || (s[i + 1] & 0xC0) != 0x80 || s[i] < 0xC2)
                return false;
            i += 2;
        } else if ((s[i] & 0xF0) == 0xE0) {
            if (i + 2 >= len || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80)
                return false;
            if (s[i] == 0xE0 && s[i + 1] < 0xA0)
                return false;
            if (s[i] == 0xED && s[i + 1] >= 0xA0)
                return false;
            i += 3;
        } else if ((s[i] & 0xF8) == 0xF0) {
            if (i + 3 >= len || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80)
                return false;
            if (s[i] == 0xF0 && s[i + 1] < 0x90)
                return false;
            if (s[i] == 0xF4 && s[i + 1] > 0x8F)
                return false;
            if (s[i] > 0xF4)
                return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::size_t utf8CodepointCount(std::string_view str) noexcept
{
    // Continuation bytes belong to the preceding sequence; every other byte
    // starts a code point for a string already known to be valid UTF-8.
    std::size_t count = 0;
    for (std::size_t i = 0; i < str.size(); ++i) {
        if ((static_cast<std::uint8_t>(str[i]) & 0xC0) != 0x80)
            ++count;
    }
    return count;
}

bool hasNoEmbeddedNul(std::string_view str) noexcept
{
    // std::string_view may contain NUL bytes even though many consumers treat
    // their input as a C string.
    return str.find('\0') == std::string_view::npos;
}

bool isValidNormalizedRect(const NormalizedRect& rect) noexcept
{
    // Require normalized coordinates at the model boundary so renderers can
    // use them without repeating finite, range, and ordering checks.
    return std::isfinite(rect.left) && std::isfinite(rect.top) && std::isfinite(rect.right)
        && std::isfinite(rect.bottom) && rect.left >= 0.0 && rect.top >= 0.0 && rect.right <= 1.0 && rect.bottom <= 1.0
        && rect.left <= rect.right && rect.top <= rect.bottom;
}

bool isValidAnnotation(const Annotation& annotation, std::string_view* reason)
{
    // Keep diagnostics as string literals so an optional reason never owns or
    // allocates memory while validating untrusted model data.
    const auto fail = [reason](std::string_view value) {
        if (reason)
            *reason = value;
        return false;
    };

    if (!isValidUtf8(annotation.contents) || !isValidUtf8(annotation.author) || !isValidUtf8(annotation.uuid))
        return fail("annotation contains invalid UTF-8 string");
    if (!hasNoEmbeddedNul(annotation.contents) || !hasNoEmbeddedNul(annotation.author)
        || !hasNoEmbeddedNul(annotation.uuid) || !hasNoEmbeddedNul(annotation.handle))
        return fail("annotation string contains embedded NUL");
    if (annotation.extras.points.size() > Limit::MaxAnnotationPoints)
        return fail("annotation points count exceeds limit");
    if (annotation.extras.quads.size() > Limit::MaxAnnotationQuads)
        return fail("annotation quads count exceeds limit");
    if (annotation.extras.inkPaths.size() > Limit::MaxAnnotationInkPaths)
        return fail("annotation ink paths count exceeds limit");
    std::size_t inkPoints = 0;
    // Check the remaining budget before adding each path to avoid size_t
    // overflow and to reject aggregate point counts above the protocol limit.
    for (const auto& path : annotation.extras.inkPaths) {
        if (path.size() > Limit::MaxAnnotationInkPoints - std::min(inkPoints, Limit::MaxAnnotationInkPoints))
            return fail("annotation ink points count exceeds limit");
        inkPoints += path.size();
    }
    if (annotation.extras.callout.size() > Limit::MaxAnnotationCalloutPoints)
        return fail("annotation callout points count exceeds limit");
    if (!Detail::isValidAnnotationExtension(annotation.extras.extension))
        return fail("annotation extension is invalid");

    return true;
}

bool isValidFormField(const FormField& field, std::string_view* reason)
{
    // Form fields combine identity, geometry, free text, and choice indices;
    // validate each group before consumers use the field in an update.
    const auto fail = [reason](std::string_view value) {
        if (reason)
            *reason = value;
        return false;
    };

    if (field.page < 0)
        return fail("form field page index is negative");
    if (field.handle.empty())
        return fail("form field handle is empty");
    if (field.handle.size() > Limit::MaxFormFieldHandleBytes)
        return fail("form field handle exceeds limit");
    if (!isValidNormalizedRect(field.rectangle))
        return fail("form field rectangle is invalid");

    const std::string_view stringsToCheck[] = { field.partialName, field.uiName, field.fullyQualifiedName,
                                                field.groupName,   field.text,   field.onState };
    for (const auto& s : stringsToCheck) {
        // All user-visible field strings share the same encoding and C-string
        // safety requirements before they reach Qt or the worker.
        if (!isValidUtf8(s))
            return fail("form field contains invalid UTF-8 string");
        if (!hasNoEmbeddedNul(s))
            return fail("form field contains string with embedded NUL byte");
    }

    if (field.partialName.size() > Limit::MaxFormNameBytes || field.uiName.size() > Limit::MaxFormNameBytes
        || field.fullyQualifiedName.size() > Limit::MaxFormNameBytes
        || field.groupName.size() > Limit::MaxFormNameBytes)
        return fail("form field name exceeds limit");

    if (field.text.size() > Limit::MaxFormFieldStringBytes)
        return fail("form field text exceeds limit");

    if (field.choices.size() > Limit::MaxFormChoices)
        return fail("form field choices count exceeds limit");

    if (!field.exportValues.empty() && field.exportValues.size() != field.choices.size())
        return fail("form field export values count does not match choices count");

    for (const auto& choice : field.choices) {
        if (!isValidUtf8(choice) || !hasNoEmbeddedNul(choice) || choice.size() > Limit::MaxFormFieldStringBytes)
            return fail("form field choice string is invalid");
    }
    for (const auto& exp : field.exportValues) {
        if (!isValidUtf8(exp) || !hasNoEmbeddedNul(exp) || exp.size() > Limit::MaxFormFieldStringBytes)
            return fail("form field export value string is invalid");
    }

    if (field.currentChoices.size() > Limit::MaxFormSelectedIndices)
        return fail("form field selected choices count exceeds limit");

    for (int idx : field.currentChoices) {
        // Negative indices are never valid; when choices exist, also require a
        // reference to an actual choice.
        if (idx < 0 || (!field.choices.empty() && static_cast<std::size_t>(idx) >= field.choices.size()))
            return fail("form field choice index out of bounds");
    }

    if (!field.multiSelect && field.currentChoices.size() > 1)
        return fail("multiple choices selected on single-select field");

    if (field.type == FormFieldType::Text && field.maximumLength < 0)
        return fail("negative text maximum length");

    return true;
}

bool isValidFormValue(const FormValue& value, std::string_view* reason)
{
    // The variant determines which limits apply. Choice indices are checked
    // for sign here; field validation performs bounds checks against choices.
    const auto fail = [reason](std::string_view message) {
        if (reason)
            *reason = message;
        return false;
    };

    return std::visit(
        [&](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, FormTextValue>) {
                if (!isValidUtf8(v.text) || !hasNoEmbeddedNul(v.text) || v.text.size() > Limit::MaxFormFieldStringBytes)
                    return fail("invalid form text value");
            } else if constexpr (std::is_same_v<T, FormChoiceSelection>) {
                if (v.selectedIndices.size() > Limit::MaxFormSelectedIndices)
                    return fail("selected indices count exceeds limit");
                for (int idx : v.selectedIndices)
                    if (idx < 0)
                        return fail("negative choice index");
            } else if constexpr (std::is_same_v<T, FormChoiceCustomText>) {
                if (!isValidUtf8(v.text) || !hasNoEmbeddedNul(v.text) || v.text.size() > Limit::MaxFormFieldStringBytes)
                    return fail("invalid form choice custom text");
            }
            return true;
        },
        value);
}

bool isValidFormUpdateRequest(const FormUpdateRequest& request, std::string_view* reason)
{
    // Validate the target handle before inspecting the replacement value so an
    // update cannot address an invalid or unbounded form field identifier.
    const auto fail = [reason](std::string_view value) {
        if (reason)
            *reason = value;
        return false;
    };

    if (request.handle.empty() || request.handle.size() > Limit::MaxFormFieldHandleBytes)
        return fail("invalid form update handle length");
    return isValidFormValue(request.value, reason);
}

bool isValidFormResetRequest(const FormResetRequest& request, std::string_view* reason)
{
    // Reset requests carry no value, so only their target handle needs checking.
    if (request.handle.empty() || request.handle.size() > Limit::MaxFormFieldHandleBytes) {
        if (reason)
            *reason = "invalid form reset handle length";
        return false;
    }
    return true;
}

} // namespace Mu::Model
