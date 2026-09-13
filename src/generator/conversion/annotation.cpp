// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/annotation.hpp"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QTimeZone>

#include <okular/core/page.h>

namespace Mu::Generator::Conversion {

static constexpr int annotationFlag(Model::AnnotationFlag flag) noexcept
{
    return Model::annotationFlagValue(flag);
}

static int pdfFlagsFor(int flags)
{
    int result = 0;
    if (flags & Okular::Annotation::Hidden)
        result |= annotationFlag(Model::AnnotationFlag::Hidden);
    if (!(flags & Okular::Annotation::DenyPrint))
        result |= annotationFlag(Model::AnnotationFlag::Print);
    if (flags & Okular::Annotation::FixedSize)
        result |= annotationFlag(Model::AnnotationFlag::NoZoom);
    if (flags & Okular::Annotation::FixedRotation)
        result |= annotationFlag(Model::AnnotationFlag::NoRotate);
    if (flags & Okular::Annotation::DenyWrite)
        result |= annotationFlag(Model::AnnotationFlag::ReadOnly);
    if (flags & Okular::Annotation::DenyDelete)
        result |= annotationFlag(Model::AnnotationFlag::Locked);
    if (flags & Okular::Annotation::ToggleHidingOnMouse)
        result |= annotationFlag(Model::AnnotationFlag::ToggleNoView);
    return result;
}

static Model::AnnotationType annotationTypeFor(const Okular::Annotation* annotation)
{
    if (const auto* text = dynamic_cast<const Okular::TextAnnotation*>(annotation))
        return text->textType() == Okular::TextAnnotation::InPlace ? Model::AnnotationType::FreeText
                                                                   : Model::AnnotationType::Text;
    if (const auto* line = dynamic_cast<const Okular::LineAnnotation*>(annotation))
        return line->linePoints().size() > 2
            ? (line->lineClosed() ? Model::AnnotationType::Polygon : Model::AnnotationType::PolyLine)
            : Model::AnnotationType::Line;
    if (const auto* geom = dynamic_cast<const Okular::GeomAnnotation*>(annotation))
        return geom->geometricalType() == Okular::GeomAnnotation::InscribedSquare ? Model::AnnotationType::Square
                                                                                  : Model::AnnotationType::Circle;
    if (const auto* highlight = dynamic_cast<const Okular::HighlightAnnotation*>(annotation)) {
        switch (highlight->highlightType()) {
        case Okular::HighlightAnnotation::Underline:
            return Model::AnnotationType::Underline;
        case Okular::HighlightAnnotation::Squiggly:
            return Model::AnnotationType::Squiggly;
        case Okular::HighlightAnnotation::StrikeOut:
            return Model::AnnotationType::StrikeOut;
        default:
            return Model::AnnotationType::Highlight;
        }
    }
    if (dynamic_cast<const Okular::InkAnnotation*>(annotation))
        return Model::AnnotationType::Ink;
    if (dynamic_cast<const Okular::StampAnnotation*>(annotation))
        return Model::AnnotationType::Stamp;
    if (dynamic_cast<const Okular::CaretAnnotation*>(annotation))
        return Model::AnnotationType::Caret;
    if (dynamic_cast<const Okular::FileAttachmentAnnotation*>(annotation))
        return Model::AnnotationType::Unknown;
    return Model::AnnotationType::Unknown;
}

static Model::Point modelPoint(const Okular::NormalizedPoint& value)
{
    return { value.x, value.y };
}

static Model::AnnotationLineEnding lineEnding(Okular::LineAnnotation::TermStyle style)
{
    switch (style) {
    case Okular::LineAnnotation::Square:
        return Model::AnnotationLineEnding::Square;
    case Okular::LineAnnotation::Circle:
        return Model::AnnotationLineEnding::Circle;
    case Okular::LineAnnotation::Diamond:
        return Model::AnnotationLineEnding::Diamond;
    case Okular::LineAnnotation::OpenArrow:
        return Model::AnnotationLineEnding::OpenArrow;
    case Okular::LineAnnotation::ClosedArrow:
        return Model::AnnotationLineEnding::ClosedArrow;
    case Okular::LineAnnotation::Butt:
        return Model::AnnotationLineEnding::Butt;
    case Okular::LineAnnotation::ROpenArrow:
        return Model::AnnotationLineEnding::ROpenArrow;
    case Okular::LineAnnotation::RClosedArrow:
        return Model::AnnotationLineEnding::RClosedArrow;
    case Okular::LineAnnotation::Slash:
        return Model::AnnotationLineEnding::Slash;
    default:
        return Model::AnnotationLineEnding::None;
    }
}

std::optional<Model::Annotation> toModel(const Okular::Annotation* annotation)
{
    if (!annotation)
        return std::nullopt;
    const Okular::NormalizedRect bounds = annotation->boundingRectangle();
    Model::Annotation data;
    const Model::AnnotationType subtype = annotationTypeFor(annotation);
    if (subtype == Model::AnnotationType::Unknown)
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
                data.extras.style.intent = Model::AnnotationIntent::FreeTextCallout;
                for (int i = 0; i < 3; ++i)
                    data.extras.callout.push_back(modelPoint(text->inplaceCallout(i)));
            } else if (text->inplaceIntent() == Okular::TextAnnotation::TypeWriter)
                data.extras.style.intent = Model::AnnotationIntent::FreeTextTypewriter;
        }
    } else if (const auto* line = dynamic_cast<const Okular::LineAnnotation*>(annotation)) {
        for (const auto& point : line->linePoints())
            data.extras.points.push_back(modelPoint(point));
        data.extras.style.closed = line->lineClosed();
        data.extras.style.firstLineEnding = lineEnding(line->lineStartStyle());
        data.extras.style.lastLineEnding = lineEnding(line->lineEndStyle());
        if (line->lineInnerColor().isValid())
            data.extras.style.interiorColor = line->lineInnerColor().rgba();
        if (line->lineIntent() == Okular::LineAnnotation::Arrow)
            data.extras.style.intent = Model::AnnotationIntent::LineArrow;
        else if (line->lineIntent() == Okular::LineAnnotation::Dimension)
            data.extras.style.intent = Model::AnnotationIntent::LineDimension;
        else if (line->lineIntent() == Okular::LineAnnotation::PolygonCloud)
            data.extras.style.intent = Model::AnnotationIntent::PolygonCloud;
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

static Okular::LineAnnotation::TermStyle lineTermStyle(Model::AnnotationLineEnding value)
{
    switch (value) {
    case Model::AnnotationLineEnding::Square:
        return Okular::LineAnnotation::Square;
    case Model::AnnotationLineEnding::Circle:
        return Okular::LineAnnotation::Circle;
    case Model::AnnotationLineEnding::Diamond:
        return Okular::LineAnnotation::Diamond;
    case Model::AnnotationLineEnding::OpenArrow:
        return Okular::LineAnnotation::OpenArrow;
    case Model::AnnotationLineEnding::ClosedArrow:
        return Okular::LineAnnotation::ClosedArrow;
    case Model::AnnotationLineEnding::Butt:
        return Okular::LineAnnotation::Butt;
    case Model::AnnotationLineEnding::ROpenArrow:
        return Okular::LineAnnotation::ROpenArrow;
    case Model::AnnotationLineEnding::RClosedArrow:
        return Okular::LineAnnotation::RClosedArrow;
    case Model::AnnotationLineEnding::Slash:
        return Okular::LineAnnotation::Slash;
    default:
        return Okular::LineAnnotation::None;
    }
}

static int okularFlagsFor(int flags)
{
    int result = 0;
    if (flags & annotationFlag(Model::AnnotationFlag::Hidden))
        result |= Okular::Annotation::Hidden;
    if (!(flags & annotationFlag(Model::AnnotationFlag::Print)))
        result |= Okular::Annotation::DenyPrint;
    if (flags & annotationFlag(Model::AnnotationFlag::NoZoom))
        result |= Okular::Annotation::FixedSize;
    if (flags & annotationFlag(Model::AnnotationFlag::NoRotate))
        result |= Okular::Annotation::FixedRotation;
    if (flags & annotationFlag(Model::AnnotationFlag::ReadOnly))
        result |= Okular::Annotation::DenyWrite;
    if (flags & annotationFlag(Model::AnnotationFlag::Locked))
        result |= Okular::Annotation::DenyDelete;
    if (flags & annotationFlag(Model::AnnotationFlag::ToggleNoView))
        result |= Okular::Annotation::ToggleHidingOnMouse;
    return result;
}

static bool isEditableAnnotationType(Model::AnnotationType type)
{
    switch (type) {
    case Model::AnnotationType::Text:
    case Model::AnnotationType::FreeText:
    case Model::AnnotationType::Line:
    case Model::AnnotationType::Square:
    case Model::AnnotationType::Circle:
    case Model::AnnotationType::Polygon:
    case Model::AnnotationType::PolyLine:
    case Model::AnnotationType::Highlight:
    case Model::AnnotationType::Underline:
    case Model::AnnotationType::Squiggly:
    case Model::AnnotationType::StrikeOut:
    case Model::AnnotationType::Stamp:
    case Model::AnnotationType::Caret:
    case Model::AnnotationType::Ink:
        return true;
    default:
        return false;
    }
}

std::unique_ptr<Okular::Annotation> fromModel(const Model::Annotation& ad)
{
    const auto& extra = ad.extras;
    const Model::AnnotationType type = ad.subtype;
    // Do not turn an unsupported PDF subtype into a TextAnnotation: doing so
    // makes a later edit silently overwrite data we cannot round-trip.
    if (!isEditableAnnotationType(type))
        return nullptr;
    Okular::Annotation* ann = nullptr;
    if (type == Model::AnnotationType::Line || type == Model::AnnotationType::Polygon
        || type == Model::AnnotationType::PolyLine) {
        auto* line = new Okular::LineAnnotation();
        QList<Okular::NormalizedPoint> points;
        for (const auto& point : extra.points)
            points.append(toNormalizedPoint(point));
        line->setLinePoints(points);
        if (extra.style.closed)
            line->setLineClosed(*extra.style.closed);
        if (extra.style.firstLineEnding)
            line->setLineStartStyle(lineTermStyle(*extra.style.firstLineEnding));
        if (extra.style.lastLineEnding)
            line->setLineEndStyle(lineTermStyle(*extra.style.lastLineEnding));
        if (extra.style.interiorColor)
            line->setLineInnerColor(QColor::fromRgba(*extra.style.interiorColor));
        const Model::AnnotationIntent intent = extra.style.intent.value_or(Model::AnnotationIntent::Default);
        if (intent == Model::AnnotationIntent::LineArrow)
            line->setLineIntent(Okular::LineAnnotation::Arrow);
        else if (intent == Model::AnnotationIntent::LineDimension)
            line->setLineIntent(Okular::LineAnnotation::Dimension);
        else if (intent == Model::AnnotationIntent::PolygonCloud)
            line->setLineIntent(Okular::LineAnnotation::PolygonCloud);
        ann = line;
    } else if (type == Model::AnnotationType::Square || type == Model::AnnotationType::Circle) {
        auto* geom = new Okular::GeomAnnotation();
        geom->setGeometricalType(type == Model::AnnotationType::Square ? Okular::GeomAnnotation::InscribedSquare
                                                                       : Okular::GeomAnnotation::InscribedCircle);
        if (extra.style.interiorColor)
            geom->setGeometricalInnerColor(QColor::fromRgba(*extra.style.interiorColor));
        ann = geom;
    } else if (type == Model::AnnotationType::Highlight || type == Model::AnnotationType::Underline
               || type == Model::AnnotationType::Squiggly || type == Model::AnnotationType::StrikeOut) {
        auto* highlight = new Okular::HighlightAnnotation();
        highlight->setHighlightType(
            type == Model::AnnotationType::Underline       ? Okular::HighlightAnnotation::Underline
                : type == Model::AnnotationType::Squiggly  ? Okular::HighlightAnnotation::Squiggly
                : type == Model::AnnotationType::StrikeOut ? Okular::HighlightAnnotation::StrikeOut
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
    } else if (type == Model::AnnotationType::Ink) {
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
    } else if (type == Model::AnnotationType::Stamp) {
        auto* stamp = new Okular::StampAnnotation();
        if (extra.style.appearance)
            stamp->setStampIconName(QString::fromStdString(extra.style.appearance->icon));
        ann = stamp;
    } else if (type == Model::AnnotationType::Caret) {
        auto* caret = new Okular::CaretAnnotation();
        if (extra.caretSymbolP)
            caret->setCaretSymbol(Okular::CaretAnnotation::CaretSymbol::P);
        ann = caret;
    } else if (type == Model::AnnotationType::Widget) {
        ann = new Okular::WidgetAnnotation();
    } else {
        auto* text = new Okular::TextAnnotation();
        text->setTextType(type == Model::AnnotationType::FreeText ? Okular::TextAnnotation::InPlace
                                                                  : Okular::TextAnnotation::Linked);
        if (extra.style.appearance)
            text->setTextIcon(QString::fromStdString(extra.style.appearance->icon));
        if (type == Model::AnnotationType::FreeText) {
            if (extra.style.appearance)
                text->setInplaceAlignment(extra.style.appearance->alignment);
            const Model::AnnotationIntent intent = extra.style.intent.value_or(Model::AnnotationIntent::Default);
            if (intent == Model::AnnotationIntent::FreeTextCallout)
                text->setInplaceIntent(Okular::TextAnnotation::Callout);
            else if (intent == Model::AnnotationIntent::FreeTextTypewriter)
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

void rebuildPageAnnotations(Okular::Page* page, const std::vector<Model::Annotation>& clean)
{
    if (!page)
        return;
    page->deleteAnnotations();
    for (const Model::Annotation& ad : clean) {
        if (auto ann = fromModel(ad))
            page->addAnnotation(ann.release());
    }
}

} // namespace Mu::Generator::Conversion
