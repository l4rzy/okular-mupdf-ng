// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/annotation.hpp"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QTimeZone>

extern "C" {
#include <mupdf/pdf.h>
}

namespace Mu::Generator::Conversion {

static int pdfFlagsFor(int flags)
{
    int result = 0;
    if (flags & Okular::Annotation::Hidden)
        result |= PDF_ANNOT_IS_HIDDEN;
    if (!(flags & Okular::Annotation::DenyPrint))
        result |= PDF_ANNOT_IS_PRINT;
    if (flags & Okular::Annotation::FixedSize)
        result |= PDF_ANNOT_IS_NO_ZOOM;
    if (flags & Okular::Annotation::FixedRotation)
        result |= PDF_ANNOT_IS_NO_ROTATE;
    if (flags & Okular::Annotation::DenyWrite)
        result |= PDF_ANNOT_IS_READ_ONLY;
    if (flags & Okular::Annotation::DenyDelete)
        result |= PDF_ANNOT_IS_LOCKED;
    if (flags & Okular::Annotation::ToggleHidingOnMouse)
        result |= PDF_ANNOT_IS_TOGGLE_NO_VIEW;
    return result;
}

static int pdfTypeFor(const Okular::Annotation* annotation)
{
    if (const auto* text = dynamic_cast<const Okular::TextAnnotation*>(annotation))
        return text->textType() == Okular::TextAnnotation::InPlace ? PDF_ANNOT_FREE_TEXT : PDF_ANNOT_TEXT;
    if (const auto* line = dynamic_cast<const Okular::LineAnnotation*>(annotation))
        return line->linePoints().size() > 2 ? (line->lineClosed() ? PDF_ANNOT_POLYGON : PDF_ANNOT_POLY_LINE)
                                             : PDF_ANNOT_LINE;
    if (const auto* geom = dynamic_cast<const Okular::GeomAnnotation*>(annotation))
        return geom->geometricalType() == Okular::GeomAnnotation::InscribedSquare ? PDF_ANNOT_SQUARE : PDF_ANNOT_CIRCLE;
    if (const auto* highlight = dynamic_cast<const Okular::HighlightAnnotation*>(annotation)) {
        switch (highlight->highlightType()) {
        case Okular::HighlightAnnotation::Underline:
            return PDF_ANNOT_UNDERLINE;
        case Okular::HighlightAnnotation::Squiggly:
            return PDF_ANNOT_SQUIGGLY;
        case Okular::HighlightAnnotation::StrikeOut:
            return PDF_ANNOT_STRIKE_OUT;
        default:
            return PDF_ANNOT_HIGHLIGHT;
        }
    }
    if (dynamic_cast<const Okular::InkAnnotation*>(annotation))
        return PDF_ANNOT_INK;
    if (dynamic_cast<const Okular::StampAnnotation*>(annotation))
        return PDF_ANNOT_STAMP;
    if (dynamic_cast<const Okular::CaretAnnotation*>(annotation))
        return PDF_ANNOT_CARET;
    if (dynamic_cast<const Okular::FileAttachmentAnnotation*>(annotation))
        return -1;
    return -1;
}

static Model::Point modelPoint(const Okular::NormalizedPoint& value)
{
    return { value.x, value.y };
}

static int lineEnding(Okular::LineAnnotation::TermStyle style)
{
    switch (style) {
    case Okular::LineAnnotation::Square:
        return PDF_ANNOT_LE_SQUARE;
    case Okular::LineAnnotation::Circle:
        return PDF_ANNOT_LE_CIRCLE;
    case Okular::LineAnnotation::Diamond:
        return PDF_ANNOT_LE_DIAMOND;
    case Okular::LineAnnotation::OpenArrow:
        return PDF_ANNOT_LE_OPEN_ARROW;
    case Okular::LineAnnotation::ClosedArrow:
        return PDF_ANNOT_LE_CLOSED_ARROW;
    case Okular::LineAnnotation::Butt:
        return PDF_ANNOT_LE_BUTT;
    case Okular::LineAnnotation::ROpenArrow:
        return PDF_ANNOT_LE_R_OPEN_ARROW;
    case Okular::LineAnnotation::RClosedArrow:
        return PDF_ANNOT_LE_R_CLOSED_ARROW;
    case Okular::LineAnnotation::Slash:
        return PDF_ANNOT_LE_SLASH;
    default:
        return PDF_ANNOT_LE_NONE;
    }
}

std::optional<Model::Annotation> toModel(const Okular::Annotation* annotation)
{
    if (!annotation)
        return std::nullopt;
    const Okular::NormalizedRect bounds = annotation->boundingRectangle();
    Model::Annotation data;
    const int subtype = pdfTypeFor(annotation);
    if (subtype < 0)
        return std::nullopt;
    data.subtype = subtype;
    data.uuid = annotation->uniqueName().toStdString();
    data.x0 = bounds.left;
    data.y0 = bounds.top;
    data.x1 = bounds.right;
    data.y1 = bounds.bottom;
    data.contents = annotation->contents().toStdString();
    data.author = annotation->author().toStdString();
    const QDateTime creationDate = annotation->creationDate();
    const QDateTime modificationDate = annotation->modificationDate();
    data.creationDate = { creationDate.isValid(), creationDate.toMSecsSinceEpoch() };
    data.modificationDate = { modificationDate.isValid(), modificationDate.toMSecsSinceEpoch() };
    const QColor col = annotation->style().color();
    const double opacity = annotation->style().opacity();
    const int alpha = std::clamp(static_cast<int>(opacity * 255.0), 0, 255);
    if (col.isValid()) {
        data.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(col.red()) << 16)
            | (static_cast<std::uint32_t>(col.green()) << 8) | static_cast<std::uint32_t>(col.blue());
    } else {
        data.color = (static_cast<std::uint32_t>(alpha > 0 ? alpha : 255) << 24);
    }
    data.flags = pdfFlagsFor(annotation->flags());
    if (const double w = annotation->style().width(); w > 0)
        data.extras.style.borderWidth = w;
    if (const auto* text = dynamic_cast<const Okular::TextAnnotation*>(annotation)) {
        data.extras.style.appearance =
            Model::AnnotationAppearance { text->textIcon().toStdString(), { }, 0, 0, 0, text->inplaceAlignment() };
        if (text->textType() == Okular::TextAnnotation::InPlace) {
            const QFont font = text->textFont();
            data.extras.style.appearance->fontName = font.family().toStdString();
            data.extras.style.appearance->fontSize = font.pointSizeF();
            const QColor txtCol = text->textColor();
            data.extras.style.appearance->textColor = txtCol.isValid() && (txtCol.rgba() & 0xffffffU) != 0
                ? (0xff000000U | (static_cast<std::uint32_t>(txtCol.red()) << 16)
                   | (static_cast<std::uint32_t>(txtCol.green()) << 8) | static_cast<std::uint32_t>(txtCol.blue()))
                : 0xff000000U;
            if (text->inplaceIntent() == Okular::TextAnnotation::Callout) {
                data.extras.style.intent = PDF_ANNOT_IT_FREETEXT_CALLOUT;
                for (int i = 0; i < 3; ++i)
                    data.extras.callout.push_back(modelPoint(text->inplaceCallout(i)));
            } else if (text->inplaceIntent() == Okular::TextAnnotation::TypeWriter)
                data.extras.style.intent = PDF_ANNOT_IT_FREETEXT_TYPEWRITER;
        }
    } else if (const auto* line = dynamic_cast<const Okular::LineAnnotation*>(annotation)) {
        for (const auto& point : line->linePoints())
            data.extras.points.push_back(modelPoint(point));
        data.extras.style.closed = line->lineClosed();
        data.extras.style.firstLineEnding =
            static_cast<Model::AnnotationLineEnding>(lineEnding(line->lineStartStyle()));
        data.extras.style.lastLineEnding = static_cast<Model::AnnotationLineEnding>(lineEnding(line->lineEndStyle()));
        if (line->lineInnerColor().isValid())
            data.extras.style.interiorColor = line->lineInnerColor().rgba();
        if (line->lineIntent() == Okular::LineAnnotation::Arrow)
            data.extras.style.intent = PDF_ANNOT_IT_LINE_ARROW;
        else if (line->lineIntent() == Okular::LineAnnotation::Dimension)
            data.extras.style.intent = PDF_ANNOT_IT_LINE_DIMENSION;
        else if (line->lineIntent() == Okular::LineAnnotation::PolygonCloud)
            data.extras.style.intent = PDF_ANNOT_IT_POLYGON_CLOUD;
    } else if (const auto* geom = dynamic_cast<const Okular::GeomAnnotation*>(annotation)) {
        if (geom->geometricalInnerColor().isValid())
            data.extras.style.interiorColor = geom->geometricalInnerColor().rgba();
    } else if (const auto* highlight = dynamic_cast<const Okular::HighlightAnnotation*>(annotation)) {
        for (const auto& quad : highlight->highlightQuads()) {
            data.extras.quads.push_back({
                modelPoint(quad.point(3)), // upperLeft
                modelPoint(quad.point(2)), // upperRight
                modelPoint(quad.point(1)), // lowerRight
                modelPoint(quad.point(0)) // lowerLeft
            });
        }
    } else if (const auto* ink = dynamic_cast<const Okular::InkAnnotation*>(annotation)) {
        for (const auto& path : ink->inkPaths()) {
            std::vector<Model::Point> values;
            for (const auto& point : path)
                values.push_back(modelPoint(point));
            data.extras.inkPaths.push_back(std::move(values));
        }
    } else if (const auto* stamp = dynamic_cast<const Okular::StampAnnotation*>(annotation)) {
        data.extras.style.appearance = Model::AnnotationAppearance { .icon = stamp->stampIconName().toStdString(),
                                                                     .fontName = { },
                                                                     .fontSize = 0,
                                                                     .textColor = 0,
                                                                     .borderWidth = 0,
                                                                     .alignment = 0 };
    } else if (const auto* caret = dynamic_cast<const Okular::CaretAnnotation*>(annotation)) {
        data.extras.caretSymbolP = caret->caretSymbol() == Okular::CaretAnnotation::CaretSymbol::P;
    }
    return data;
}

static Okular::NormalizedPoint toNormalizedPoint(const Model::Point& value)
{
    return Okular::NormalizedPoint(value.x, value.y);
}

static Okular::LineAnnotation::TermStyle lineTermStyle(int value)
{
    switch (value) {
    case PDF_ANNOT_LE_SQUARE:
        return Okular::LineAnnotation::Square;
    case PDF_ANNOT_LE_CIRCLE:
        return Okular::LineAnnotation::Circle;
    case PDF_ANNOT_LE_DIAMOND:
        return Okular::LineAnnotation::Diamond;
    case PDF_ANNOT_LE_OPEN_ARROW:
        return Okular::LineAnnotation::OpenArrow;
    case PDF_ANNOT_LE_CLOSED_ARROW:
        return Okular::LineAnnotation::ClosedArrow;
    case PDF_ANNOT_LE_BUTT:
        return Okular::LineAnnotation::Butt;
    case PDF_ANNOT_LE_R_OPEN_ARROW:
        return Okular::LineAnnotation::ROpenArrow;
    case PDF_ANNOT_LE_R_CLOSED_ARROW:
        return Okular::LineAnnotation::RClosedArrow;
    case PDF_ANNOT_LE_SLASH:
        return Okular::LineAnnotation::Slash;
    default:
        return Okular::LineAnnotation::None;
    }
}

static int okularFlagsFor(int flags)
{
    int result = 0;
    if (flags & PDF_ANNOT_IS_HIDDEN)
        result |= Okular::Annotation::Hidden;
    if (!(flags & PDF_ANNOT_IS_PRINT))
        result |= Okular::Annotation::DenyPrint;
    if (flags & PDF_ANNOT_IS_NO_ZOOM)
        result |= Okular::Annotation::FixedSize;
    if (flags & PDF_ANNOT_IS_NO_ROTATE)
        result |= Okular::Annotation::FixedRotation;
    if (flags & PDF_ANNOT_IS_READ_ONLY)
        result |= Okular::Annotation::DenyWrite;
    if (flags & PDF_ANNOT_IS_LOCKED)
        result |= Okular::Annotation::DenyDelete;
    if (flags & PDF_ANNOT_IS_TOGGLE_NO_VIEW)
        result |= Okular::Annotation::ToggleHidingOnMouse;
    return result;
}

static bool isEditableAnnotationType(int type)
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

std::unique_ptr<Okular::Annotation> fromModel(const Model::Annotation& ad)
{
    const auto& extra = ad.extras;
    const int type = ad.subtype;
    // Do not turn an unsupported PDF subtype into a TextAnnotation: doing so
    // makes a later edit silently overwrite data we cannot round-trip.
    if (!isEditableAnnotationType(type))
        return nullptr;
    Okular::Annotation* ann = nullptr;
    if (type == PDF_ANNOT_LINE || type == PDF_ANNOT_POLYGON || type == PDF_ANNOT_POLY_LINE) {
        auto* line = new Okular::LineAnnotation();
        QList<Okular::NormalizedPoint> points;
        for (const auto& point : extra.points)
            points.append(toNormalizedPoint(point));
        line->setLinePoints(points);
        if (extra.style.closed)
            line->setLineClosed(*extra.style.closed);
        if (extra.style.firstLineEnding)
            line->setLineStartStyle(lineTermStyle(static_cast<int>(*extra.style.firstLineEnding)));
        if (extra.style.lastLineEnding)
            line->setLineEndStyle(lineTermStyle(static_cast<int>(*extra.style.lastLineEnding)));
        if (extra.style.interiorColor)
            line->setLineInnerColor(QColor::fromRgba(*extra.style.interiorColor));
        const int intent = extra.style.intent.value_or(0);
        if (intent == PDF_ANNOT_IT_LINE_ARROW)
            line->setLineIntent(Okular::LineAnnotation::Arrow);
        else if (intent == PDF_ANNOT_IT_LINE_DIMENSION)
            line->setLineIntent(Okular::LineAnnotation::Dimension);
        else if (intent == PDF_ANNOT_IT_POLYGON_CLOUD)
            line->setLineIntent(Okular::LineAnnotation::PolygonCloud);
        ann = line;
    } else if (type == PDF_ANNOT_SQUARE || type == PDF_ANNOT_CIRCLE) {
        auto* geom = new Okular::GeomAnnotation();
        geom->setGeometricalType(type == PDF_ANNOT_SQUARE ? Okular::GeomAnnotation::InscribedSquare
                                                          : Okular::GeomAnnotation::InscribedCircle);
        if (extra.style.interiorColor)
            geom->setGeometricalInnerColor(QColor::fromRgba(*extra.style.interiorColor));
        ann = geom;
    } else if (type == PDF_ANNOT_HIGHLIGHT || type == PDF_ANNOT_UNDERLINE || type == PDF_ANNOT_SQUIGGLY
               || type == PDF_ANNOT_STRIKE_OUT) {
        auto* highlight = new Okular::HighlightAnnotation();
        highlight->setHighlightType(type == PDF_ANNOT_UNDERLINE        ? Okular::HighlightAnnotation::Underline
                                        : type == PDF_ANNOT_SQUIGGLY   ? Okular::HighlightAnnotation::Squiggly
                                        : type == PDF_ANNOT_STRIKE_OUT ? Okular::HighlightAnnotation::StrikeOut
                                                                       : Okular::HighlightAnnotation::Highlight);
        for (const auto& values : extra.quads) {
            Okular::HighlightAnnotation::Quad quad;
            quad.setPoint(toNormalizedPoint(values.lowerLeft), 0);
            quad.setPoint(toNormalizedPoint(values.lowerRight), 1);
            quad.setPoint(toNormalizedPoint(values.upperRight), 2);
            quad.setPoint(toNormalizedPoint(values.upperLeft), 3);
            highlight->highlightQuads().append(quad);
        }
        ann = highlight;
    } else if (type == PDF_ANNOT_INK) {
        auto* ink = new Okular::InkAnnotation();
        QList<QList<Okular::NormalizedPoint>> paths;
        for (const auto& rawPath : extra.inkPaths) {
            QList<Okular::NormalizedPoint> path;
            for (const auto& point : rawPath)
                path.append(toNormalizedPoint(point));
            paths.append(path);
        }
        ink->setInkPaths(paths);
        ann = ink;
    } else if (type == PDF_ANNOT_STAMP) {
        auto* stamp = new Okular::StampAnnotation();
        if (extra.style.appearance)
            stamp->setStampIconName(QString::fromStdString(extra.style.appearance->icon));
        ann = stamp;
    } else if (type == PDF_ANNOT_CARET) {
        auto* caret = new Okular::CaretAnnotation();
        if (extra.caretSymbolP)
            caret->setCaretSymbol(Okular::CaretAnnotation::CaretSymbol::P);
        ann = caret;
    } else if (type == PDF_ANNOT_WIDGET) {
        ann = new Okular::WidgetAnnotation();
    } else {
        auto* text = new Okular::TextAnnotation();
        text->setTextType(type == PDF_ANNOT_FREE_TEXT ? Okular::TextAnnotation::InPlace
                                                      : Okular::TextAnnotation::Linked);
        if (extra.style.appearance)
            text->setTextIcon(QString::fromStdString(extra.style.appearance->icon));
        if (type == PDF_ANNOT_FREE_TEXT) {
            if (extra.style.appearance)
                text->setInplaceAlignment(extra.style.appearance->alignment);
            const int intent = extra.style.intent.value_or(0);
            if (intent == PDF_ANNOT_IT_FREETEXT_CALLOUT)
                text->setInplaceIntent(Okular::TextAnnotation::Callout);
            else if (intent == PDF_ANNOT_IT_FREETEXT_TYPEWRITER)
                text->setInplaceIntent(Okular::TextAnnotation::TypeWriter);
            QFont font(extra.style.appearance ? QString::fromStdString(extra.style.appearance->fontName) : QString());
            if (extra.style.appearance && extra.style.appearance->fontSize > 0)
                font.setPointSizeF(extra.style.appearance->fontSize);
            if (!font.family().isEmpty())
                text->setTextFont(font);
            if (extra.style.appearance)
                text->setTextColor(QColor::fromRgba(extra.style.appearance->textColor));
            for (int i = 0; i < static_cast<int>(extra.callout.size()) && i < 3; ++i)
                text->setInplaceCallout(toNormalizedPoint(extra.callout[static_cast<std::size_t>(i)]), i);
        }
        ann = text;
    }
    if (!ann)
        return nullptr;
    ann->setContents(QString::fromStdString(ad.contents));
    ann->setAuthor(QString::fromStdString(ad.author));
    if (ad.creationDate.valid)
        ann->setCreationDate(QDateTime::fromMSecsSinceEpoch(ad.creationDate.unixMilliseconds, QTimeZone::UTC));
    if (ad.modificationDate.valid)
        ann->setModificationDate(QDateTime::fromMSecsSinceEpoch(ad.modificationDate.unixMilliseconds, QTimeZone::UTC));
    ann->setUniqueName(QString::fromStdString(ad.uuid));
    Okular::NormalizedRect bb(ad.x0, ad.y0, ad.x1, ad.y1);
    ann->setBoundingRectangle(bb);
    ann->style().setColor(QColor::fromRgb((ad.color >> 16) & 0xff, (ad.color >> 8) & 0xff, ad.color & 0xff));
    ann->style().setOpacity(static_cast<double>(ad.color >> 24) / 255.0);
    if (extra.style.borderWidth && *extra.style.borderWidth > 0)
        ann->style().setWidth(*extra.style.borderWidth);
    // MuPDF renders native annotations into worker frames. Mark them as
    // externally drawn so Okular invalidates the cached page raster after an
    // add, edit, or removal instead of waiting for a later viewport redraw.
    ann->setFlags(okularFlagsFor(ad.flags) | Okular::Annotation::ExternallyDrawn);
    ann->setNativeId(QString::fromStdString(ad.handle));
    return std::unique_ptr<Okular::Annotation>(ann);
}

} // namespace Mu::Generator::Conversion
