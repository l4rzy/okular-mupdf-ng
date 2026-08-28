// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/epub/document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "engine/constants.hpp"
#include "shared/logging.hpp"
#include "shared/model/validation.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

namespace {

struct PageDimensions {
    float widthPoints;
    float heightPoints;
};

/// Computes paper dimensions in points (72 DPI) for standard EPUB paper presets.
constexpr PageDimensions pageDimensions(EpubPageSize pageSize) noexcept
{
    switch (pageSize) {
    case EpubPageSize::A5:
        return { 148.0f * Constant::MillimetersToPoints, 210.0f * Constant::MillimetersToPoints };
    case EpubPageSize::SixByNine:
        return { 152.0f * Constant::MillimetersToPoints, 229.0f * Constant::MillimetersToPoints };
    case EpubPageSize::Letter:
        return { 216.0f * Constant::MillimetersToPoints, 279.0f * Constant::MillimetersToPoints };
    case EpubPageSize::B5:
    default:
        return { 176.0f * Constant::MillimetersToPoints, 250.0f * Constant::MillimetersToPoints };
    }
}

/// Generates font family CSS override rules.
constexpr const char* fontFamilyCss(EpubFontFamily fontFamily) noexcept
{
    switch (fontFamily) {
    case EpubFontFamily::Serif:
        return "body, body * { font-family: serif !important; }";
    case EpubFontFamily::SansSerif:
        return "body, body * { font-family: sans-serif !important; }";
    case EpubFontFamily::Monospace:
        return "body, body * { font-family: monospace !important; }";
    case EpubFontFamily::Default:
    default:
        return nullptr;
    }
}

/// Decodes base64-encoded user CSS and validates UTF-8 sanity.
std::optional<std::string> decodeCustomCss(fz_context* context, std::string_view encoded)
{
    if (encoded.empty())
        return std::string();
    if (encoded.size() > MaxEpubCustomCssBase64Bytes || encoded.size() % 4 != 0)
        return std::nullopt;

    const auto isBase64 = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
    };
    std::size_t padding = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (c == '=') {
            ++padding;
            if (i < encoded.size() - 2 || padding > 2)
                return std::nullopt;
        } else if (!isBase64(c) || padding != 0) {
            return std::nullopt;
        }
    }

    std::optional<std::string> result;
    fz_buffer* decoded = nullptr;
    fz_try(context)
    {
        decoded = fz_new_buffer_from_base64(context, encoded.data(), encoded.size());
        unsigned char* data = nullptr;
        const std::size_t decodedSize = fz_buffer_storage(context, decoded, &data);
        const std::string_view cssView(reinterpret_cast<const char*>(data), decodedSize);
        result = std::string(cssView);

        if (result && (!isValidUtf8(*result) || utf8CodepointCount(*result) > MaxEpubCustomCssCharacters))
            result.reset();
    }
    fz_always(context)
    {
        if (decoded)
            fz_drop_buffer(context, decoded);
    }
    fz_catch(context)
    {
        return std::nullopt;
    }
    return result;
}

/// Constructs composite CSS string for EPUB styling injection.
std::string documentCss(const DocumentSettings& settings, std::string_view customCss)
{
    const auto dimensions = pageDimensions(settings.epub.pageSize);
    const float marginX = dimensions.widthPoints * Constant::PageMarginFraction;
    const float marginY = dimensions.heightPoints * Constant::PageMarginFraction;

    std::array<char, 128> marginRule { };
    std::snprintf(marginRule.data(), marginRule.size(), "body { margin: %.3fpt %.3fpt !important; }", marginY, marginX);

    // Some EPUBs specify both width and height as 100% for images. During
    // reflow, the containing block may not have a definite height, causing
    // the image to collapse or overlap nearby text. Keep the image's natural
    // aspect ratio and cap it at the available content width, matching the
    // image safeguards used by SumatraPDF.
    std::string css = "img { height: auto !important; max-width: 100% !important; } ";
    css += marginRule.data();
    if (const char* fontRule = fontFamilyCss(settings.epub.fontFamily)) {
        css += ' ';
        css += fontRule;
    }
    if (!customCss.empty()) {
        css += ' ';
        css += customCss;
    }
    return css;
}

} // namespace

// =============================================================================
// Construction, Destruction & Context Lifetime
// =============================================================================

EpubDocument::EpubDocument(std::size_t storeSize)
    : m_context(fz_new_context(nullptr, nullptr, storeSize))
{
    if (m_context)
        fz_register_document_handlers(m_context);
}

EpubDocument::~EpubDocument()
{
    close();
    if (m_context)
        fz_drop_context(m_context);
}

// =============================================================================
// Document Opening, Styling & Pagination
// =============================================================================

bool EpubDocument::openFd(int fd, std::string displayName, std::string* error)
{
    return openFdWithAccelerator(fd, std::move(displayName), { }, error);
}

bool EpubDocument::openFdWithAccelerator(int fd,
                                         std::string displayName,
                                         const std::vector<std::uint8_t>& accelerator,
                                         std::string* error)
{
    close();
    if (!m_context || fd < 0)
        return fail(error, "input FD is invalid");
    m_input = ::fdopen(fd, "rb");
    if (!m_input) {
        ::close(fd);
        return fail(error, "could not adopt input FD");
    }

    const auto customCss = decodeCustomCss(m_context, m_settings.epub.customCssBase64);
    const bool hasCustomCss = customCss.has_value() && !customCss->empty();
    const std::string css = documentCss(m_settings, customCss.value_or(std::string()));
    const std::string fallbackCss = hasCustomCss ? documentCss(m_settings, { }) : std::string();

    fz_buffer* acceleratorBuffer = nullptr;
    fz_stream* acceleratorStream = nullptr;
    fz_var(acceleratorBuffer);
    fz_var(acceleratorStream);
    fz_try(m_context)
    {
        // Step 1: Open stream and instantiate MuPDF document handle
        m_stream = fz_open_file_ptr_no_close(m_context, m_input);
        if (!accelerator.empty()) {
            acceleratorBuffer = fz_new_buffer_from_copied_data(m_context, accelerator.data(), accelerator.size());
            acceleratorStream = fz_open_buffer(m_context, acceleratorBuffer);
            m_document =
                fz_open_accelerated_document_with_stream(m_context, displayName.c_str(), m_stream, acceleratorStream);
        } else {
            m_document = fz_open_document_with_stream(m_context, displayName.c_str(), m_stream);
        }

        // Step 2: Inject custom styling rules (margins, font-family, custom CSS)
        fz_try(m_context)
        {
            fz_style_document(m_context, m_document, 1, css.c_str());
        }
        fz_catch(m_context)
        {
            if (!hasCustomCss)
                fz_rethrow(m_context);
            MU_LOG(warning, "Mu::Worker::Epub", "custom CSS was rejected; using built-in EPUB CSS");
            fz_style_document(m_context, m_document, 1, fallbackCss.c_str());
        }

        // Step 3: Perform initial layout and pagination pass
        const auto layout = layoutGeometry();
        if (!layoutTo(layout.paperWidth, layout.paperHeight, static_cast<float>(m_settings.epub.fontSize)))
            fz_throw(m_context, FZ_ERROR_GENERIC, "EPUB layout failed");
    }
    fz_always(m_context)
    {
        if (acceleratorStream)
            fz_drop_stream(m_context, acceleratorStream);
        if (acceleratorBuffer)
            fz_drop_buffer(m_context, acceleratorBuffer);
    }
    fz_catch(m_context)
    {
        close();
        return fail(error, fz_caught_message(m_context));
    }
    m_displayName = std::move(displayName);
    return true;
}

std::vector<std::uint8_t> EpubDocument::exportAccelerator(std::string* error) const
{
    if (!m_context || !m_document || !fz_document_supports_accelerator(m_context, m_document))
        return { };

    fz_buffer* buffer = nullptr;
    std::vector<std::uint8_t> result;
    fz_var(buffer);
    fz_try(m_context)
    {
        buffer = fz_new_buffer(m_context, 0);
        fz_output_accelerator(m_context, m_document, fz_new_output_with_buffer(m_context, buffer));
        unsigned char* data = nullptr;
        const std::size_t size = fz_buffer_storage(m_context, buffer, &data);
        if (size > MaxEpubAcceleratorBytes)
            fz_throw(m_context, FZ_ERROR_LIMIT, "EPUB accelerator exceeds limit");
        result.assign(data, data + size);
    }
    fz_always(m_context)
    {
        if (buffer)
            fz_drop_buffer(m_context, buffer);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    return result;
}

bool EpubDocument::unlock(const std::string&, std::string* error)
{
    return fail(error, "EPUB document is not password locked");
}

void EpubDocument::close() noexcept
{
    if (m_document && m_context) {
        fz_try(m_context)
        {
            fz_drop_document(m_context, m_document);
        }
        fz_catch(m_context)
        {
        }
    }
    m_document = nullptr;
    if (m_archive && m_context) {
        fz_try(m_context)
        {
            fz_drop_archive(m_context, m_archive);
        }
        fz_catch(m_context)
        {
        }
    }
    m_archive = nullptr;
    m_fonts.reset();
    if (m_stream && m_context) {
        fz_try(m_context)
        {
            fz_drop_stream(m_context, m_stream);
        }
        fz_catch(m_context)
        {
        }
    }
    m_stream = nullptr;
    if (m_input)
        ::fclose(m_input);
    m_input = nullptr;
    m_pageCount = 0;
    m_layoutWidth = 0;
    m_layoutHeight = 0;
    m_displayName.clear();

    trimProcessMemory(m_context);
}

bool EpubDocument::isOpen() const noexcept
{
    return m_document != nullptr;
}

bool EpubDocument::isLocked() const noexcept
{
    return false;
}

int EpubDocument::pageCount() const noexcept
{
    return m_pageCount;
}

// =============================================================================
// Layout Geometry & Pagination Mechanics
// =============================================================================

EpubDocument::LayoutGeometry EpubDocument::layoutGeometry() const noexcept
{
    // Settings changes take effect on the next open. While a document is
    // open, report the dimensions used by its current MuPDF layout instead
    // of the pending settings stored for the next document.
    if (m_document && m_layoutWidth > 0.0f && m_layoutHeight > 0.0f)
        return { m_layoutWidth, m_layoutHeight };

    const auto dimensions = pageDimensions(m_settings.epub.pageSize);
    return { dimensions.widthPoints, dimensions.heightPoints };
}

bool EpubDocument::layoutTo(float widthPoints, float heightPoints, float fontSize) const
{
    if (!m_document || !m_context)
        return false;
    if (std::abs(m_layoutWidth - widthPoints) < 1.0f && std::abs(m_layoutHeight - heightPoints) < 1.0f)
        return true;

    // The caller owns the surrounding fz_try block. Let MuPDF exceptions
    // reach it so opening cannot succeed with a stale or empty page count.
    fz_layout_document(m_context, m_document, widthPoints, heightPoints, fontSize);
    m_pageCount = fz_count_pages(m_context, m_document);
    m_layoutWidth = widthPoints;
    m_layoutHeight = heightPoints;
    return true;
}

// =============================================================================
// Internal Page Loading Helpers
// =============================================================================

fz_page* EpubDocument::loadPage(int page, std::string* error) const
{
    if (!m_document || page < 0 || page >= m_pageCount) {
        fail(error, "page is unavailable");
        return nullptr;
    }
    fz_page* result = nullptr;
    fz_var(result);
    fz_try(m_context)
    {
        result = fz_load_page(m_context, m_document, page);
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
    }
    return result;
}

fz_page* EpubDocument::loadPageWithBounds(int page, fz_rect* bounds, std::string* error) const
{
    if (!bounds) {
        fail(error, "page bounds output is null");
        return nullptr;
    }

    fz_page* result = loadPage(page, error);
    if (!result)
        return nullptr;

    bool boundsLoaded = false;
    fz_var(result);
    fz_var(boundsLoaded);
    fz_try(m_context)
    {
        *bounds = fz_bound_page(m_context, result);
        boundsLoaded = true;
    }
    fz_always(m_context)
    {
        if (!boundsLoaded) {
            fz_drop_page(m_context, result);
            result = nullptr;
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
    }
    return result;
}

DocumentSettings EpubDocument::settings() const noexcept
{
    return m_settings;
}

void EpubDocument::setSettings(const DocumentSettings& settings) noexcept
{
    m_settings = settings;
    m_settings.epub.fontSize = std::clamp(m_settings.epub.fontSize, std::int32_t(10), std::int32_t(20));
    if (static_cast<std::uint8_t>(m_settings.epub.pageSize) > static_cast<std::uint8_t>(EpubPageSize::Letter))
        m_settings.epub.pageSize = EpubPageSize::B5;
    if (static_cast<std::uint8_t>(m_settings.epub.fontFamily) > static_cast<std::uint8_t>(EpubFontFamily::Monospace))
        m_settings.epub.fontFamily = EpubFontFamily::Default;
    applyFitzSettings(m_context, m_settings);
}

} // namespace Mu::Worker::Engine
