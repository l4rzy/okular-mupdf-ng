// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

namespace {

void normalizeAnnotationForWrite(Annotation& annotation)
{
    const auto coordinate = [](double value) {
        return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
    };
    const auto point = [&coordinate](Point& value) {
        value.x = coordinate(value.x);
        value.y = coordinate(value.y);
    };
    annotation.x0 = coordinate(annotation.x0);
    annotation.y0 = coordinate(annotation.y0);
    annotation.x1 = coordinate(annotation.x1);
    annotation.y1 = coordinate(annotation.y1);
    if (annotation.x0 > annotation.x1)
        std::swap(annotation.x0, annotation.x1);
    if (annotation.y0 > annotation.y1)
        std::swap(annotation.y0, annotation.y1);

    const auto normalizePoints = [&point](std::vector<Point>& values, std::size_t limit) {
        values.resize(std::min(values.size(), limit));
        for (auto& value : values)
            point(value);
    };
    normalizePoints(annotation.extras.points, Limit::MaxAnnotationPoints);
    normalizePoints(annotation.extras.callout, Limit::MaxAnnotationCalloutPoints);
    annotation.extras.quads.resize(std::min(annotation.extras.quads.size(), Limit::MaxAnnotationQuads));
    for (auto& quad : annotation.extras.quads) {
        point(quad.upperLeft);
        point(quad.upperRight);
        point(quad.lowerRight);
        point(quad.lowerLeft);
    }

    annotation.extras.inkPaths.resize(std::min(annotation.extras.inkPaths.size(), Limit::MaxAnnotationInkPaths));
    std::size_t remaining = Limit::MaxAnnotationInkPoints;
    for (auto& path : annotation.extras.inkPaths) {
        path.resize(std::min(path.size(), remaining));
        remaining -= path.size();
        for (auto& value : path)
            point(value);
    }
    if (annotation.extras.style.borderWidth) {
        const double value = *annotation.extras.style.borderWidth;
        *annotation.extras.style.borderWidth = std::isfinite(value) ? std::max(0.0, value) : 0.0;
    }
    if (annotation.extras.style.appearance) {
        auto& appearance = *annotation.extras.style.appearance;
        appearance.fontSize = std::isfinite(appearance.fontSize) ? std::max(0.0, appearance.fontSize) : 0.0;
        appearance.borderWidth = std::isfinite(appearance.borderWidth) ? std::max(0.0, appearance.borderWidth) : 0.0;
    }
}

/// Coordinate conversion adapter between absolute PDF user points and Okular normalized [0, 1] page fractions.
struct PageCoordinates {
    fz_rect bounds;
    float width;
    float height;

    [[nodiscard]] constexpr Point fromPdfPoint(fz_point value) const noexcept
    {
        return { (value.x - bounds.x0) / width, (value.y - bounds.y0) / height };
    }

    [[nodiscard]] constexpr fz_point toPdfPoint(const Point& value) const noexcept
    {
        return { bounds.x0 + static_cast<float>(value.x * width), bounds.y0 + static_cast<float>(value.y * height) };
    }

    [[nodiscard]] constexpr fz_rect toPdfRect(const Annotation& value) const noexcept
    {
        return { bounds.x0 + static_cast<float>(value.x0 * width),
                 bounds.y0 + static_cast<float>(value.y0 * height),
                 bounds.x0 + static_cast<float>(value.x1 * width),
                 bounds.y0 + static_cast<float>(value.y1 * height) };
    }
};

/// Clamps geometry point counts to prevent malicious PDFs from causing memory exhaustion.
constexpr int boundedGeometryCount(int count) noexcept
{
    return std::clamp(count, 0, Constant::MaxAnnotationGeometryPoints);
}

/// Returns true if an annotation subtype is a text markup annotation (highlight, underline, etc.).
constexpr bool isMarkupAnnotation(enum pdf_annot_type type) noexcept
{
    return type == PDF_ANNOT_HIGHLIGHT || type == PDF_ANNOT_UNDERLINE || type == PDF_ANNOT_SQUIGGLY
        || type == PDF_ANNOT_STRIKE_OUT;
}

/// Converts PDF float color components [0.0, 1.0] and opacity to 32-bit ARGB integer.
std::uint32_t pdfColorToArgb(int components, const float values[4], float opacity)
{
    const auto channel = [](float value) -> std::uint32_t {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };

    float red = 0, green = 0, blue = 0;
    if (components == 1)
        red = green = blue = values[0];
    else if (components == 3)
        red = values[0], green = values[1], blue = values[2];
    else
        return 0xffffffffU;

    return (channel(opacity) << 24) | (channel(red) << 16) | (channel(green) << 8) | channel(blue);
}

/// Extracts float channel intensity [0.0, 1.0] from an 8-bit ARGB channel component.
float argbChannel(std::uint32_t value)
{
    return static_cast<float>(value & 0xffU) / 255.0f;
}

} // namespace

// =============================================================================
// Transactional Annotation Operations
// =============================================================================

// Higher-order helper for exception-safe annotation operations. Loads the native page,
// iterates over the page's annotation linked list looking for objectNumber, executes
// callback(pdfPage, targetAnnot), and guarantees fz_drop_page cleanup via fz_always.
template <typename Callback>
bool PdfDocument::withAnnotation(int page, std::int32_t objectNumber, std::string* error, Callback&& callback)
{
    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return false;

    bool found = false;
    fz_try(m_context)
    {
        pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
        for (pdf_annot* annotation = pdfPage ? pdf_first_annot(m_context, pdfPage) : nullptr; annotation;
             annotation = pdf_next_annot(m_context, annotation)) {
            if (pdf_to_num(m_context, pdf_annot_obj(m_context, annotation)) == objectNumber) {
                callback(pdfPage, annotation);
                found = true;
                break;
            }
        }
        if (!found)
            fz_throw(m_context, FZ_ERROR_ARGUMENT, "annotation not found");
    }
    fz_always(m_context)
    {
        fz_drop_page(m_context, nativePage);
    }
    fz_catch(m_context)
    {
        return fail(error, fz_caught_message(m_context));
    }

    return true;
}

// Transactional annotation creation: pdf_create_annot allocates the annotation object.
// If applyAnnotation throws an exception, complete remains false and fz_always rolls
// back by deleting the partial annotation from pdfPage and dropping its reference.
bool PdfDocument::addAnnotation(int page, const Annotation& annotation, std::int32_t* objectNumber, std::string* error)
{
    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return false;

    Annotation normalized = annotation;
    normalizeAnnotationForWrite(normalized);
    bool complete = false;
    pdf_annot* created = nullptr;
    fz_var(complete);
    fz_var(created);
    fz_try(m_context)
    {
        pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
        if (!pdfPage || !isEditableAnnotation(annotation.subtype))
            fz_throw(m_context, FZ_ERROR_ARGUMENT, "annotation type is not editable");

        created = pdf_create_annot(m_context, pdfPage, static_cast<enum pdf_annot_type>(annotation.subtype));

        applyAnnotation(m_context, created, normalized, fz_bound_page(m_context, nativePage), true);

        const int number = pdf_to_num(m_context, pdf_annot_obj(m_context, created));
        if (number <= 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "annotation has no PDF object");

        if (objectNumber)
            *objectNumber = number;

        complete = true;
    }
    fz_always(m_context)
    {
        if (created) {
            if (!complete) {
                // Rollback is best effort. Keep a failure in rollback from
                // replacing the original annotation operation error.
                fz_try(m_context)
                {
                    pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
                    if (pdfPage)
                        pdf_delete_annot(m_context, pdfPage, created);
                }
                fz_catch(m_context)
                {
                    fz_ignore_error(m_context);
                }
            }
            pdf_drop_annot(m_context, created);
        }
        fz_drop_page(m_context, nativePage);
    }
    fz_catch(m_context)
    {
        return fail(error, fz_caught_message(m_context));
    }

    return true;
}

bool PdfDocument::modifyAnnotation(
    int page, std::int32_t objectNumber, const Annotation& annotation, bool appearanceChanged, std::string* error)
{
    Annotation normalized = annotation;
    normalizeAnnotationForWrite(normalized);
    return withAnnotation(page, objectNumber, error, [&](pdf_page* pdfPage, pdf_annot* target) {
        if (!isEditableAnnotation(pdf_annot_type(m_context, target))
            || annotation.subtype != pdf_annot_type(m_context, target))
            fz_throw(m_context, FZ_ERROR_ARGUMENT, "annotation type mismatch");

        applyAnnotation(
            m_context, target, normalized, pdf_bound_page(m_context, pdfPage, FZ_CROP_BOX), appearanceChanged);
    });
}

bool PdfDocument::removeAnnotation(int page, std::int32_t objectNumber, std::string* error)
{
    return withAnnotation(page, objectNumber, error, [&](pdf_page* pdfPage, pdf_annot* target) {
        pdf_delete_annot(m_context, pdfPage, target);
    });
}

std::vector<Annotation>
PdfDocument::extractPageAnnotations(fz_page* nativePage, const fz_rect& bounds, std::string* error) const
{
    // All annotation geometry is reported to the model as normalized page coordinates.
    const PageCoordinates coordinates { bounds, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0 };
    std::vector<Annotation> result;
    Annotation value;
    pdf_page* pdfPage = nullptr;
    pdf_annot* firstAnnot = nullptr;
    fz_try(m_context)
    {
        pdfPage = pdf_page_from_fz_page(m_context, nativePage);
        if (!pdfPage)
            fz_throw(m_context, FZ_ERROR_ARGUMENT, "page is not a PDF page");
        firstAnnot = pdf_first_annot(m_context, pdfPage);
        if (!(coordinates.width > 0 && coordinates.height > 0))
            fz_throw(m_context, FZ_ERROR_GENERIC, "page has invalid bounds");

        int index = 0;
        for (pdf_annot* annotation = firstAnnot; annotation;
             annotation = pdf_next_annot(m_context, annotation), ++index) {
            if (result.size() >= Constant::MaxPageAnnotations)
                fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: page annotation limit exceeded");

            value = { };
            fz_try(m_context)
            {
                // Read fields shared by every annotation before decoding type-specific data.
                const auto type = pdf_annot_type(m_context, annotation);
                if (type == PDF_ANNOT_WIDGET)
                    continue;

                value.subtype = static_cast<std::int32_t>(type);
                value.nativeIndex = index;
                value.pdfObjectNumber = pdf_to_num(m_context, pdf_annot_obj(m_context, annotation));
                value.flags = pdf_annot_flags(m_context, annotation);

                if (const char* text = pdf_annot_name(m_context, annotation))
                    value.uuid = text;
                if (const char* text = pdf_annot_contents(m_context, annotation))
                    value.contents = text;
                if (const char* text = pdf_annot_author(m_context, annotation))
                    value.author = text;

                const auto creation = pdf_annot_creation_date(m_context, annotation);
                const auto modification = pdf_annot_modification_date(m_context, annotation);
                if (creation > 0)
                    value.creationDate = { true, creation * 1000 };
                if (modification > 0)
                    value.modificationDate = { true, modification * 1000 };

                const fz_rect rectangle = pdf_bound_annot(m_context, annotation);
                const Point topLeft = coordinates.fromPdfPoint({ rectangle.x0, rectangle.y0 });
                const Point bottomRight = coordinates.fromPdfPoint({ rectangle.x1, rectangle.y1 });
                value.x0 = topLeft.x;
                value.y0 = topLeft.y;
                value.x1 = bottomRight.x;
                value.y1 = bottomRight.y;

                int components = 0;
                float color[4] { 0, 0, 0, 1 };
                pdf_annot_color(m_context, annotation, &components, color);
                value.color = pdfColorToArgb(components, color, pdf_annot_opacity(m_context, annotation));

                if (pdf_annot_has_interior_color(m_context, annotation)) {
                    int interiorComponents = 0;
                    float interior[4] { 0, 0, 0, 1 };
                    pdf_annot_interior_color(m_context, annotation, &interiorComponents, interior);
                    value.extras.style.interiorColor = pdfColorToArgb(interiorComponents, interior, 1.0f);
                }
                if (pdf_annot_has_border(m_context, annotation)) {
                    const float border = pdf_annot_border_width(m_context, annotation);
                    if (border > 0)
                        value.extras.style.borderWidth = border;
                }
                if (pdf_annot_has_icon_name(m_context, annotation)) {
                    if (const char* icon = pdf_annot_icon_name(m_context, annotation)) {
                        value.extras.style.appearance.emplace();
                        value.extras.style.appearance->icon = icon;
                    }
                }
                if (pdf_annot_has_intent(m_context, annotation))
                    value.extras.style.intent = static_cast<std::int32_t>(pdf_annot_intent(m_context, annotation));
                if (type == PDF_ANNOT_CARET) {
                    value.extras.caretSymbolP = pdf_name_eq(
                        m_context, pdf_dict_gets(m_context, pdf_annot_obj(m_context, annotation), "Sy"), PDF_NAME(P));
                }

                // Geometry is represented differently by each annotation family, but all
                // coordinates pass through the same PDF-to-normalized-page conversion.
                switch (type) {
                case PDF_ANNOT_HIGHLIGHT:
                case PDF_ANNOT_UNDERLINE:
                case PDF_ANNOT_SQUIGGLY:
                case PDF_ANNOT_STRIKE_OUT: {
                    // Markup annotations store one quadrilateral per highlighted region.
                    const int count = boundedGeometryCount(pdf_annot_quad_point_count(m_context, annotation));
                    value.extras.quads.reserve(static_cast<std::size_t>(count));
                    for (int quad = 0; quad < count; ++quad) {
                        const fz_quad raw = pdf_annot_quad_point(m_context, annotation, quad);
                        value.extras.quads.push_back({ coordinates.fromPdfPoint(raw.ul),
                                                       coordinates.fromPdfPoint(raw.ur),
                                                       coordinates.fromPdfPoint(raw.lr),
                                                       coordinates.fromPdfPoint(raw.ll) });
                    }
                    break;
                }
                case PDF_ANNOT_INK: {
                    // Ink annotations contain multiple strokes, each with its own vertex list.
                    const int strokes = boundedGeometryCount(pdf_annot_ink_list_count(m_context, annotation));
                    value.extras.inkPaths.reserve(static_cast<std::size_t>(strokes));
                    for (int stroke = 0; stroke < strokes; ++stroke) {
                        const int count =
                            boundedGeometryCount(pdf_annot_ink_list_stroke_count(m_context, annotation, stroke));
                        value.extras.inkPaths.emplace_back();
                        auto& path = value.extras.inkPaths.back();
                        path.reserve(static_cast<std::size_t>(count));
                        for (int vertex = 0; vertex < count; ++vertex)
                            path.push_back(coordinates.fromPdfPoint(
                                pdf_annot_ink_list_stroke_vertex(m_context, annotation, stroke, vertex)));
                    }
                    break;
                }
                case PDF_ANNOT_LINE:
                case PDF_ANNOT_POLYGON:
                case PDF_ANNOT_POLY_LINE:
                    // Lines use two endpoints; polygons and polylines use a vertex list.
                    if (pdf_annot_has_line_ending_styles(m_context, annotation)) {
                        value.extras.style.firstLineEnding =
                            static_cast<AnnotationLineEnding>(pdf_annot_line_start_style(m_context, annotation));
                        value.extras.style.lastLineEnding =
                            static_cast<AnnotationLineEnding>(pdf_annot_line_end_style(m_context, annotation));
                    }
                    value.extras.style.closed = (type == PDF_ANNOT_POLYGON);
                    if (type == PDF_ANNOT_LINE) {
                        fz_point first { }, second { };
                        pdf_annot_line(m_context, annotation, &first, &second);
                        value.extras.points = { coordinates.fromPdfPoint(first), coordinates.fromPdfPoint(second) };
                    } else {
                        const int count = boundedGeometryCount(pdf_annot_vertex_count(m_context, annotation));
                        value.extras.points.reserve(static_cast<std::size_t>(count));
                        for (int vertex = 0; vertex < count; ++vertex)
                            value.extras.points.push_back(
                                coordinates.fromPdfPoint(pdf_annot_vertex(m_context, annotation, vertex)));
                    }
                    break;
                case PDF_ANNOT_FREE_TEXT: {
                    // Free-text annotations use their default appearance for font, color,
                    // alignment, and may additionally contain a two- or three-point callout.
                    const char* font = nullptr;
                    float fontSize = 0;
                    int textComponents = 0;
                    float textColor[4] { 0, 0, 0, 1 };
                    pdf_annot_default_appearance(m_context, annotation, &font, &fontSize, &textComponents, textColor);
                    value.extras.style.appearance.emplace();
                    auto& appearance = *value.extras.style.appearance;
                    if (font)
                        appearance.fontName = font;
                    appearance.fontSize = fontSize;
                    if (textComponents > 0)
                        appearance.textColor = pdfColorToArgb(textComponents, textColor, 1.0f);
                    if (pdf_annot_has_quadding(m_context, annotation))
                        appearance.alignment = pdf_annot_quadding(m_context, annotation);
                    if (pdf_annot_has_callout(m_context, annotation)) {
                        fz_point points[3] { };
                        int count = 0;
                        pdf_annot_callout_line(m_context, annotation, points, &count);
                        for (int point = 0; point < count; ++point)
                            value.extras.callout.push_back(coordinates.fromPdfPoint(points[point]));
                    }
                    break;
                }
                default:
                    break;
                }

                // Commit only fully parsed annotations; malformed annotations are handled
                // by the catch below and do not discard the rest of the page.
                result.push_back(std::move(value));
            }
            fz_catch(m_context)
            {
                // Annotation extraction is intentionally best effort per object;
                // malformed geometry must not hide valid annotations on the page.
            }
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return { };
    }
    return result;
}

// =============================================================================
// Annotation Type Validation
// =============================================================================

constexpr bool PdfDocument::isEditableAnnotation(std::int32_t type) noexcept
{
    switch (type) {
    case PDF_ANNOT_TEXT:
    case PDF_ANNOT_FREE_TEXT:
    case PDF_ANNOT_LINE:
    case PDF_ANNOT_SQUARE:
    case PDF_ANNOT_CIRCLE:
    case PDF_ANNOT_POLYGON:
    case PDF_ANNOT_POLY_LINE:
    case PDF_ANNOT_HIGHLIGHT:
    case PDF_ANNOT_UNDERLINE:
    case PDF_ANNOT_SQUIGGLY:
    case PDF_ANNOT_STRIKE_OUT:
    case PDF_ANNOT_STAMP:
    case PDF_ANNOT_CARET:
    case PDF_ANNOT_INK:
        return true;
    default:
        return false;
    }
}

// =============================================================================
// Property Application & Coordinate Mapping
// =============================================================================

// Maps normalized [0,1] page coordinates to absolute PDF user-space points, applies styling
// (color, opacity, border), and writes geometry (rects, quads, vertices, ink lists).
void PdfDocument::applyAnnotation(fz_context* context,
                                  pdf_annot* target,
                                  const Annotation& annotation,
                                  const fz_rect& pageBounds,
                                  bool updateAppearance)
{
    fz_quad* quads = nullptr;
    fz_point* linePoints = nullptr;
    int* inkCounts = nullptr;
    fz_point* inkPoints = nullptr;
    fz_var(quads);
    fz_var(linePoints);
    fz_var(inkCounts);
    fz_var(inkPoints);
    fz_try(context)
    {
        const float pageWidth = pageBounds.x1 - pageBounds.x0;
        const float pageHeight = pageBounds.y1 - pageBounds.y0;

        const PageCoordinates coordinates { pageBounds, pageWidth, pageHeight };

        if (!annotation.uuid.empty())
            pdf_set_annot_name(context, target, annotation.uuid.c_str());

        pdf_set_annot_contents(context, target, annotation.contents.c_str());
        pdf_set_annot_author(context, target, annotation.author.c_str());

        if (annotation.creationDate.valid)
            pdf_set_annot_creation_date(context, target, annotation.creationDate.unixMilliseconds / 1000);
        if (annotation.modificationDate.valid)
            pdf_set_annot_modification_date(context, target, annotation.modificationDate.unixMilliseconds / 1000);

        const float rawOpacity = argbChannel(annotation.color >> 24);
        const float opacity = rawOpacity > 0.0f ? rawOpacity : 1.0f;
        const float color[3] { argbChannel(annotation.color >> 16),
                               argbChannel(annotation.color >> 8),
                               argbChannel(annotation.color) };

        if ((annotation.color >> 24) > 0)
            pdf_set_annot_color(context, target, 3, color);

        pdf_set_annot_opacity(context, target, opacity);
        pdf_set_annot_flags(context, target, annotation.flags);

        const auto type = pdf_annot_type(context, target);
        const bool markup = isMarkupAnnotation(type);

        const auto point = [&coordinates](const Point& value) {
            return coordinates.toPdfPoint(value);
        };
        const fz_rect normalizedRect = coordinates.toPdfRect(annotation);

        if (pdf_annot_has_rect(context, target))
            pdf_set_annot_rect(context, target, normalizedRect);

        if (!markup && annotation.extras.style.borderWidth && pdf_annot_has_border(context, target))
            pdf_set_annot_border_width(context, target, static_cast<float>(*annotation.extras.style.borderWidth));

        if (annotation.extras.style.appearance && !annotation.extras.style.appearance->icon.empty()
            && pdf_annot_has_icon_name(context, target))
            pdf_set_annot_icon_name(context, target, annotation.extras.style.appearance->icon.c_str());

        if (annotation.extras.style.intent)
            pdf_set_annot_intent(context, target, static_cast<enum pdf_intent>(*annotation.extras.style.intent));

        if (type == PDF_ANNOT_CARET) {
            pdf_obj* annotationObject = pdf_annot_obj(context, target);
            if (annotation.extras.caretSymbolP)
                pdf_dict_puts_drop(context, annotationObject, "Sy", pdf_new_name(context, "P"));
            else
                pdf_dict_dels(context, annotationObject, "Sy");
        }

        if (annotation.extras.style.interiorColor) {
            const auto value = *annotation.extras.style.interiorColor;
            const float interior[] { argbChannel(value >> 16), argbChannel(value >> 8), argbChannel(value) };
            pdf_set_annot_interior_color(context, target, 3, interior);
        }

        if (annotation.extras.style.firstLineEnding && annotation.extras.style.lastLineEnding)
            pdf_set_annot_line_ending_styles(
                context,
                target,
                static_cast<enum pdf_line_ending>(*annotation.extras.style.firstLineEnding),
                static_cast<enum pdf_line_ending>(*annotation.extras.style.lastLineEnding));

        // PDF 1.7 Specification (Table 174): QuadPoints require 8 numbers per quad in the order:
        // top-left, top-right, bottom-left, bottom-right.
        if (markup && !annotation.extras.quads.empty()) {
            quads = fz_malloc_array(context, annotation.extras.quads.size(), fz_quad);
            for (std::size_t index = 0; index < annotation.extras.quads.size(); ++index) {
                const Quad& quad = annotation.extras.quads[index];
                const fz_point ul = point(quad.upperLeft), ur = point(quad.upperRight);
                const fz_point lr = point(quad.lowerRight), ll = point(quad.lowerLeft);
                quads[index] = { ul, ur, ll, lr };
            }
            pdf_set_annot_quad_points(context, target, static_cast<int>(annotation.extras.quads.size()), quads);
        }

        if (type == PDF_ANNOT_LINE || type == PDF_ANNOT_POLYGON || type == PDF_ANNOT_POLY_LINE) {
            if (annotation.extras.points.size() == 2)
                pdf_set_annot_line(
                    context, target, point(annotation.extras.points[0]), point(annotation.extras.points[1]));
            else if (annotation.extras.points.size() > 2) {
                linePoints = fz_malloc_array(context, annotation.extras.points.size(), fz_point);
                for (std::size_t index = 0; index < annotation.extras.points.size(); ++index)
                    linePoints[index] = point(annotation.extras.points[index]);
                pdf_set_annot_vertices(context, target, static_cast<int>(annotation.extras.points.size()), linePoints);
            }
        } else if (type == PDF_ANNOT_INK && !annotation.extras.inkPaths.empty()) {
            // Flatten multi-stroke ink paths into contiguous count and vertex arrays for pdf_set_annot_ink_list.
            std::size_t pointCount = 0;
            for (const auto& path : annotation.extras.inkPaths)
                pointCount += path.size();
            inkCounts = fz_malloc_array(context, annotation.extras.inkPaths.size(), int);
            inkPoints = pointCount ? fz_malloc_array(context, pointCount, fz_point) : nullptr;
            std::size_t pointIndex = 0;
            for (std::size_t pathIndex = 0; pathIndex < annotation.extras.inkPaths.size(); ++pathIndex) {
                const auto& path = annotation.extras.inkPaths[pathIndex];
                inkCounts[pathIndex] = static_cast<int>(path.size());
                for (const auto& value : path)
                    inkPoints[pointIndex++] = point(value);
            }
            pdf_set_annot_ink_list(
                context, target, static_cast<int>(annotation.extras.inkPaths.size()), inkCounts, inkPoints);
        }

        if (type == PDF_ANNOT_FREE_TEXT) {
            const char* fontName = "Helv";
            float size = 12.0f;
            float textColor[3] { 0, 0, 0 };
            if (annotation.extras.style.appearance) {
                const auto& app = *annotation.extras.style.appearance;
                if (!app.fontName.empty()) {
                    if (app.fontName == "Courier" || app.fontName == "monospace")
                        fontName = "Cour";
                    else if (app.fontName == "Times" || app.fontName == "Times New Roman" || app.fontName == "serif")
                        fontName = "TiRo";
                    else
                        fontName = "Helv";
                }
                if (app.fontSize > 0)
                    size = static_cast<float>(app.fontSize);
                if (app.textColor != 0) {
                    textColor[0] = argbChannel(app.textColor >> 16);
                    textColor[1] = argbChannel(app.textColor >> 8);
                    textColor[2] = argbChannel(app.textColor);
                }
                if (pdf_annot_has_quadding(context, target))
                    pdf_set_annot_quadding(context, target, app.alignment);
            }
            pdf_set_annot_default_appearance(context, target, fontName, size, 3, textColor);
        }

        if (type == PDF_ANNOT_FREE_TEXT && !annotation.extras.callout.empty() && annotation.extras.style.intent
            && *annotation.extras.style.intent == PDF_ANNOT_IT_FREETEXT_CALLOUT) {
            fz_point pts[3] { };
            const int n = std::min(static_cast<int>(annotation.extras.callout.size()), 3);
            for (int i = 0; i < n; ++i)
                pts[i] = point(annotation.extras.callout[static_cast<std::size_t>(i)]);
            pdf_set_annot_callout_line(context, target, pts, n);
        }

        // Synthesize visual appearance stream (/AP) for the annotation
        if (updateAppearance)
            pdf_update_annot(context, target);
    }
    fz_always(context)
    {
        fz_free(context, quads);
        fz_free(context, linePoints);
        fz_free(context, inkCounts);
        fz_free(context, inkPoints);
    }
    fz_catch(context)
    {
        fz_rethrow(context);
    }
}

} // namespace Mu::Worker::Engine
