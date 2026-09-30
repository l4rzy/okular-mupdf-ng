// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/xfdf/export.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

#include "shared/model/geometry.hpp"

namespace Mu::Plugin::Xfdf {

namespace {

/// Maps one normalized top-left page point to PDF user-space (origin bottom-left)
/// for a page of the given size. The axis flip is owned by the shared model
/// geometry helpers so import and export stay exact inverses.
Model::Point toUserSpace(double x, double y, double pageWidth, double pageHeight)
{
    return Model::normalizedToUserSpace({ x, y }, pageWidth, pageHeight);
}

QString formatNumber(double value)
{
    // XFDF coordinates are decimal; six places keeps sub-pixel precision without
    // trailing noise, and -0 collapses to 0. Non-finite values would serialize
    // as invalid XML tokens, so they collapse to 0 as well.
    if (!std::isfinite(value) || std::abs(value) < 1e-9)
        value = 0;
    QString text = QString::number(value, 'f', 6);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text.isEmpty() ? QStringLiteral("0") : text;
}

QString escapeXml(const QString& value)
{
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar ch : value) {
        const char16_t code = ch.unicode();
        switch (code) {
        case u'&':
            escaped += QStringLiteral("&amp;");
            break;
        case u'<':
            escaped += QStringLiteral("&lt;");
            break;
        case u'>':
            escaped += QStringLiteral("&gt;");
            break;
        case u'"':
            escaped += QStringLiteral("&quot;");
            break;
        case u'\'':
            escaped += QStringLiteral("&apos;");
            break;
        default:
            // XML 1.0 forbids C0 control characters except tab, LF and CR.
            // Drop them rather than emit a document that fails to parse.
            if (code < 0x20 && code != u'\t' && code != u'\n' && code != u'\r')
                break;
            escaped += ch;
        }
    }
    return escaped;
}

QString colorAttribute(std::uint32_t argb)
{
    return QStringLiteral("#%1%2%3")
        .arg((argb >> 16) & 0xff, 2, 16, QLatin1Char('0'))
        .arg((argb >> 8) & 0xff, 2, 16, QLatin1Char('0'))
        .arg(argb & 0xff, 2, 16, QLatin1Char('0'));
}

QString opacityAttribute(std::uint32_t argb)
{
    const double opacity = static_cast<double>(argb >> 24) / 255.0;
    return formatNumber(opacity);
}

QString xfdfDate(const Model::Timestamp& timestamp)
{
    if (!timestamp.valid)
        return { };
    // XFDF dates use the PDF "D:YYYYMMDDHHmmSSOHH'mm" syntax.
    const QDateTime dateTime = QDateTime::fromMSecsSinceEpoch(timestamp.unixMilliseconds, QTimeZone::UTC);
    return QStringLiteral("D:%1+00'00'").arg(dateTime.toString(QStringLiteral("yyyyMMddHHmmss")));
}

QString lineEndingName(Model::AnnotationLineEnding ending)
{
    switch (ending) {
    case Model::AnnotationLineEnding::Square:
        return QStringLiteral("Square");
    case Model::AnnotationLineEnding::Circle:
        return QStringLiteral("Circle");
    case Model::AnnotationLineEnding::Diamond:
        return QStringLiteral("Diamond");
    case Model::AnnotationLineEnding::OpenArrow:
        return QStringLiteral("OpenArrow");
    case Model::AnnotationLineEnding::ClosedArrow:
        return QStringLiteral("ClosedArrow");
    case Model::AnnotationLineEnding::Butt:
        return QStringLiteral("Butt");
    case Model::AnnotationLineEnding::ROpenArrow:
        return QStringLiteral("ROpenArrow");
    case Model::AnnotationLineEnding::RClosedArrow:
        return QStringLiteral("RClosedArrow");
    case Model::AnnotationLineEnding::Slash:
        return QStringLiteral("Slash");
    default:
        return QStringLiteral("None");
    }
}

QString intentName(Model::AnnotationIntent intent)
{
    switch (intent) {
    case Model::AnnotationIntent::FreeTextCallout:
        return QStringLiteral("FreeTextCallout");
    case Model::AnnotationIntent::FreeTextTypewriter:
        return QStringLiteral("FreeTextTypewriter");
    case Model::AnnotationIntent::LineArrow:
        return QStringLiteral("LineArrow");
    case Model::AnnotationIntent::LineDimension:
        return QStringLiteral("LineDimension");
    case Model::AnnotationIntent::PolyLineDimension:
        return QStringLiteral("PolyLineDimension");
    case Model::AnnotationIntent::PolygonCloud:
        return QStringLiteral("PolygonCloud");
    case Model::AnnotationIntent::PolygonDimension:
        return QStringLiteral("PolygonDimension");
    case Model::AnnotationIntent::StampImage:
        return QStringLiteral("StampImage");
    case Model::AnnotationIntent::StampSnapshot:
        return QStringLiteral("StampSnapshot");
    default:
        return { };
    }
}

/// Appends the XFDF comma-separated list of flags set in the annotation bitmask.
void appendFlagAttributes(QString& out, std::int32_t flags)
{
    struct FlagName {
        Model::AnnotationFlag flag;
        const char* name;
    };

    static constexpr FlagName names[] = {
        { Model::AnnotationFlag::Hidden, "hidden" },
        { Model::AnnotationFlag::Print, "print" },
        { Model::AnnotationFlag::NoZoom, "nozoom" },
        { Model::AnnotationFlag::NoRotate, "norotate" },
        { Model::AnnotationFlag::ReadOnly, "readonly" },
        { Model::AnnotationFlag::Locked, "locked" },
        { Model::AnnotationFlag::ToggleNoView, "togglenoview" },
    };
    QStringList values;
    for (const auto& entry : names) {
        if (flags & Model::annotationFlagValue(entry.flag))
            values.append(QLatin1String(entry.name));
    }
    if (!values.isEmpty())
        out += QStringLiteral(" flags=\"%1\"").arg(values.join(QLatin1Char(',')));
}

QString pointPairs(const std::vector<Model::Point>& points, double pageWidth, double pageHeight)
{
    QStringList parts;
    parts.reserve(static_cast<qsizetype>(points.size()));
    for (const Model::Point& point : points) {
        const Model::Point user = toUserSpace(point.x, point.y, pageWidth, pageHeight);
        parts.append(QStringLiteral("%1,%2").arg(formatNumber(user.x), formatNumber(user.y)));
    }
    return parts.join(QLatin1Char(';'));
}

QString coordinateList(const std::vector<Model::Point>& points, double pageWidth, double pageHeight)
{
    QStringList parts;
    parts.reserve(static_cast<qsizetype>(points.size()) * 2);
    for (const Model::Point& point : points) {
        const Model::Point user = toUserSpace(point.x, point.y, pageWidth, pageHeight);
        parts << formatNumber(user.x) << formatNumber(user.y);
    }
    return parts.join(QLatin1Char(','));
}

QString pdfName(const std::string& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    const QByteArray bytes = QString::fromStdString(value).toUtf8();
    QString out = QStringLiteral("/");
    for (const char rawByte : bytes) {
        const auto byte = static_cast<unsigned char>(rawByte);
        const bool needsEscape = byte <= 0x20 || byte >= 0x7f || byte == '(' || byte == ')' || byte == '<'
            || byte == '>' || byte == '[' || byte == ']' || byte == '{' || byte == '}' || byte == '/' || byte == '%'
            || byte == '#';
        if (needsEscape) {
            out += QLatin1Char('#');
            out += QLatin1Char(hex[byte >> 4]);
            out += QLatin1Char(hex[byte & 0x0f]);
        } else {
            out += QLatin1Char(static_cast<char>(byte));
        }
    }
    return out;
}

QString rectAttribute(const Model::Quad& rect)
{
    return QStringLiteral("%1,%2,%3,%4")
        .arg(formatNumber(rect.upperLeft.x),
             formatNumber(rect.lowerLeft.y),
             formatNumber(rect.lowerRight.x),
             formatNumber(rect.upperRight.y));
}

/// Serializes the common attributes shared by every XFDF annotation element.
QString commonAttributes(const Model::Annotation& annotation, int pageIndex, const Model::Quad& rect)
{
    QString out = QStringLiteral(" page=\"%1\" rect=\"%2\"").arg(pageIndex).arg(rectAttribute(rect));
    out += QStringLiteral(" color=\"%1\"").arg(colorAttribute(annotation.color));
    out += QStringLiteral(" opacity=\"%1\"").arg(opacityAttribute(annotation.color));
    if (!annotation.author.empty())
        out += QStringLiteral(" title=\"%1\"").arg(escapeXml(QString::fromStdString(annotation.author)));
    if (!annotation.uuid.empty())
        out += QStringLiteral(" name=\"%1\"").arg(escapeXml(QString::fromStdString(annotation.uuid)));
    const QString creationDate = xfdfDate(annotation.creationDate);
    if (!creationDate.isEmpty())
        out += QStringLiteral(" creationdate=\"%1\"").arg(creationDate);
    const QString date =
        xfdfDate(annotation.modificationDate.valid ? annotation.modificationDate : annotation.creationDate);
    if (!date.isEmpty())
        out += QStringLiteral(" date=\"%1\"").arg(date);
    appendFlagAttributes(out, annotation.flags);
    return out;
}

QString contentsElement(const Model::Annotation& annotation)
{
    if (annotation.contents.empty())
        return { };
    return QStringLiteral("<contents>%1</contents>").arg(escapeXml(QString::fromStdString(annotation.contents)));
}

/// Appends one quad in XFDF/Acrobat `coords` order. XFDF expects
/// top-left, top-right, bottom-left, bottom-right, while Model::Quad stores
/// clockwise upperLeft, upperRight, lowerRight, lowerLeft, so the trailing
/// pair is swapped.
void appendXfdfQuad(std::vector<Model::Point>& out, const Model::Quad& quad)
{
    out.push_back(quad.upperLeft);
    out.push_back(quad.upperRight);
    out.push_back(quad.lowerLeft);
    out.push_back(quad.lowerRight);
}

/// Writes one annotation element. Returns an empty string for subtypes that the
/// XFDF mapping does not cover.
QString annotationElement(const Model::Annotation& annotation, int pageIndex, const Page& geometry)
{
    const Model::Quad rect = normalizedRectToUserSpace(annotation, geometry.widthPoints, geometry.heightPoints);
    const QString common = commonAttributes(annotation, pageIndex, rect);
    const QString contents = contentsElement(annotation);
    const auto& style = annotation.extras.style;

    switch (annotation.subtype) {
    case Model::AnnotationType::Text:
    case Model::AnnotationType::FreeText: {
        const bool freeText = annotation.subtype == Model::AnnotationType::FreeText;
        const bool hasCallout = freeText && !annotation.extras.callout.empty()
            && (!style.intent || *style.intent == Model::AnnotationIntent::Default
                || *style.intent == Model::AnnotationIntent::FreeTextCallout);
        QString inner = contents;
        if (style.appearance && !style.appearance->fontName.empty())
            inner += QStringLiteral("<defaultappearance>0 0 0 rg %1 %2 Tf</defaultappearance>")
                         .arg(escapeXml(pdfName(style.appearance->fontName)), formatNumber(style.appearance->fontSize));
        QString out =
            QStringLiteral("<%1%2").arg(freeText ? QStringLiteral("freetext") : QStringLiteral("text")).arg(common);
        if (hasCallout)
            out += QStringLiteral(" callout=\"%1\"")
                       .arg(coordinateList(annotation.extras.callout, geometry.widthPoints, geometry.heightPoints));
        if (style.appearance && !style.appearance->icon.empty())
            out += QStringLiteral(" icon=\"%1\"").arg(escapeXml(QString::fromStdString(style.appearance->icon)));
        if (style.intent || hasCallout) {
            const QString intent = style.intent && *style.intent != Model::AnnotationIntent::Default
                ? intentName(*style.intent)
                : hasCallout ? QStringLiteral("FreeTextCallout")
                             : QString();
            if (!intent.isEmpty())
                out += QStringLiteral(" intent=\"%1\"").arg(intent);
        }
        out += QLatin1Char('>') + inner
            + QStringLiteral("</%1>").arg(freeText ? QStringLiteral("freetext") : QStringLiteral("text"));
        return out;
    }
    case Model::AnnotationType::Line:
    case Model::AnnotationType::Polygon:
    case Model::AnnotationType::PolyLine: {
        const bool polygon = annotation.subtype == Model::AnnotationType::Polygon;
        const bool polyLine = annotation.subtype == Model::AnnotationType::PolyLine;
        QString out = QStringLiteral("<%1%2")
                          .arg(polygon        ? QStringLiteral("polygon")
                                   : polyLine ? QStringLiteral("polyline")
                                              : QStringLiteral("line"))
                          .arg(common);
        if (const auto& width = style.borderWidth)
            out += QStringLiteral(" width=\"%1\"").arg(formatNumber(*width));
        if (style.intent) {
            const QString intent = intentName(*style.intent);
            if (!intent.isEmpty())
                out += QStringLiteral(" intent=\"%1\"").arg(intent);
        }
        if (polygon || polyLine) {
            out += QLatin1Char('>');
            out += QStringLiteral("<vertices>%1</vertices>")
                       .arg(pointPairs(annotation.extras.points, geometry.widthPoints, geometry.heightPoints));
        } else {
            if (annotation.extras.points.size() < 2)
                return { };
            const Model::Point first = toUserSpace(annotation.extras.points.front().x,
                                                   annotation.extras.points.front().y,
                                                   geometry.widthPoints,
                                                   geometry.heightPoints);
            const Model::Point last = toUserSpace(annotation.extras.points.back().x,
                                                  annotation.extras.points.back().y,
                                                  geometry.widthPoints,
                                                  geometry.heightPoints);
            out += QStringLiteral(" start=\"%1,%2\" end=\"%3,%4\"")
                       .arg(formatNumber(first.x), formatNumber(first.y), formatNumber(last.x), formatNumber(last.y));
            if (style.firstLineEnding)
                out += QStringLiteral(" tail=\"%1\"").arg(lineEndingName(*style.firstLineEnding));
            if (style.lastLineEnding)
                out += QStringLiteral(" head=\"%1\"").arg(lineEndingName(*style.lastLineEnding));
            out += QLatin1Char('>');
        }
        out += contents
            + QStringLiteral("</%1>").arg(polygon        ? QStringLiteral("polygon")
                                              : polyLine ? QStringLiteral("polyline")
                                                         : QStringLiteral("line"));
        return out;
    }
    case Model::AnnotationType::Square:
    case Model::AnnotationType::Circle: {
        const bool square = annotation.subtype == Model::AnnotationType::Square;
        QString out =
            QStringLiteral("<%1%2").arg(square ? QStringLiteral("square") : QStringLiteral("circle")).arg(common);
        if (const auto& width = style.borderWidth)
            out += QStringLiteral(" width=\"%1\"").arg(formatNumber(*width));
        out += QLatin1Char('>') + contents
            + QStringLiteral("</%1>").arg(square ? QStringLiteral("square") : QStringLiteral("circle"));
        return out;
    }
    case Model::AnnotationType::Highlight:
    case Model::AnnotationType::Underline:
    case Model::AnnotationType::Squiggly:
    case Model::AnnotationType::StrikeOut: {
        QString name = QStringLiteral("highlight");
        if (annotation.subtype == Model::AnnotationType::Underline)
            name = QStringLiteral("underline");
        else if (annotation.subtype == Model::AnnotationType::Squiggly)
            name = QStringLiteral("squiggly");
        else if (annotation.subtype == Model::AnnotationType::StrikeOut)
            name = QStringLiteral("strikeout");
        QString out = QStringLiteral("<%1%2").arg(name).arg(common);
        std::vector<Model::Point> points;
        for (const Model::Quad& quad : annotation.extras.quads)
            appendXfdfQuad(points, quad);
        if (!points.empty())
            out += QStringLiteral(" coords=\"%1\"")
                       .arg(coordinateList(points, geometry.widthPoints, geometry.heightPoints));
        out += QLatin1Char('>');
        out += contents + QStringLiteral("</%1>").arg(name);
        return out;
    }
    case Model::AnnotationType::Ink: {
        QString out = QStringLiteral("<ink%1>").arg(common);
        if (!annotation.extras.inkPaths.empty())
            out += QStringLiteral("<inklist>");
        for (const auto& path : annotation.extras.inkPaths) {
            out += QStringLiteral("<gesture>%1</gesture>")
                       .arg(pointPairs(path, geometry.widthPoints, geometry.heightPoints));
        }
        if (!annotation.extras.inkPaths.empty())
            out += QStringLiteral("</inklist>");
        out += contents + QStringLiteral("</ink>");
        return out;
    }
    case Model::AnnotationType::Stamp: {
        QString out = QStringLiteral("<stamp%1").arg(common);
        if (style.appearance && !style.appearance->icon.empty())
            out += QStringLiteral(" icon=\"%1\"").arg(escapeXml(QString::fromStdString(style.appearance->icon)));
        return out + QLatin1Char('>') + contents + QStringLiteral("</stamp>");
    }
    case Model::AnnotationType::Caret: {
        QString out = QStringLiteral("<caret%1").arg(common);
        if (annotation.extras.caretSymbolP)
            out += QStringLiteral(" symbol=\"P\"");
        return out + QLatin1Char('>') + contents + QStringLiteral("</caret>");
    }
    default:
        return { };
    }
}

/// Tight normalized bounding box in Okular's top-left, Y-down space.
struct NormalizedBounds {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
};

void extendBounds(NormalizedBounds& bounds, bool& valid, const Model::Point& point)
{
    if (!valid) {
        bounds = { point.x, point.y, point.x, point.y };
        valid = true;
        return;
    }
    bounds.x0 = std::min(bounds.x0, point.x);
    bounds.y0 = std::min(bounds.y0, point.y);
    bounds.x1 = std::max(bounds.x1, point.x);
    bounds.y1 = std::max(bounds.y1, point.y);
}

/// Derives the XFDF `rect` source for an annotation. The worker reports `x0..y1`
/// from MuPDF's padded display bounds, so for subtypes carrying explicit
/// geometry the tighter point/quad/ink union is preferred and `x0..y1` is only
/// the fallback.
NormalizedBounds normalizedBoundsOf(const Model::Annotation& annotation)
{
    NormalizedBounds bounds { annotation.x0, annotation.y0, annotation.x1, annotation.y1 };
    bool valid = false;
    switch (annotation.subtype) {
    case Model::AnnotationType::Highlight:
    case Model::AnnotationType::Underline:
    case Model::AnnotationType::Squiggly:
    case Model::AnnotationType::StrikeOut:
        for (const Model::Quad& quad : annotation.extras.quads) {
            extendBounds(bounds, valid, quad.upperLeft);
            extendBounds(bounds, valid, quad.upperRight);
            extendBounds(bounds, valid, quad.lowerRight);
            extendBounds(bounds, valid, quad.lowerLeft);
        }
        break;
    case Model::AnnotationType::Line:
    case Model::AnnotationType::Polygon:
    case Model::AnnotationType::PolyLine:
        for (const Model::Point& point : annotation.extras.points)
            extendBounds(bounds, valid, point);
        break;
    case Model::AnnotationType::Ink:
        for (const auto& path : annotation.extras.inkPaths) {
            for (const Model::Point& point : path)
                extendBounds(bounds, valid, point);
        }
        break;
    case Model::AnnotationType::FreeText:
        // XFDF rect describes the text box; its leader is serialized separately.
        valid = true;
        break;
    default:
        break;
    }
    return bounds;
}

} // namespace

Model::Quad normalizedRectToUserSpace(const Model::Annotation& annotation, double pageWidth, double pageHeight)
{
    const NormalizedBounds bounds = normalizedBoundsOf(annotation);
    const Model::Point topLeft = toUserSpace(bounds.x0, bounds.y0, pageWidth, pageHeight);
    const Model::Point bottomRight = toUserSpace(bounds.x1, bounds.y1, pageWidth, pageHeight);
    return { topLeft, { bottomRight.x, topLeft.y }, bottomRight, { topLeft.x, bottomRight.y } };
}

QString annotationsToXfdf(const QVector<Page>& pages)
{
    QString out = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    out += QStringLiteral("<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n");
    out += QStringLiteral("<annots>\n");

    for (int pageIndex = 0; pageIndex < pages.size(); ++pageIndex) {
        const Page& page = pages.at(pageIndex);
        for (const Model::Annotation& annotation : page.annotations) {
            const QString element = annotationElement(annotation, pageIndex, page);
            if (!element.isEmpty())
                out += element + QLatin1Char('\n');
        }
    }

    out += QStringLiteral("</annots>\n");
    out += QStringLiteral("</xfdf>\n");
    return out;
}

} // namespace Mu::Plugin::Xfdf
