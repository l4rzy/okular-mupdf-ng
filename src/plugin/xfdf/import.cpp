// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/xfdf/import.hpp"

#include <QChar>
#include <QDate>
#include <QDateTime>
#include <QStringList>
#include <QTime>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "shared/model/geometry.hpp"
#include "shared/protocol/limits.hpp"

namespace Mu::Plugin::Xfdf {

namespace {

using Model::Annotation;
using Model::AnnotationAppearance;
using Model::AnnotationIntent;
using Model::AnnotationLineEnding;
using Model::AnnotationType;
using Model::Point;
using Model::Quad;
using Model::Timestamp;

/// Bounds the diagnostic list so a hostile document cannot grow it without end.
constexpr int MaxWarnings = 20;

enum class GeometryParseStatus { Complete, Malformed, LimitExceeded };

std::optional<double> parseNumber(QStringView text)
{
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value))
        return std::nullopt;
    return value;
}

/// Maps one PDF user-space point (origin bottom-left) to normalized top-left
/// page coordinates. The axis flip is owned by the shared model geometry
/// helpers, which are the exact inverse of the exporter's conversion.
Point normalizedPoint(double x, double y, double width, double height)
{
    return Model::userSpaceToNormalized({ x, y }, width, height);
}

void parseSinglePoint(QStringView text, double width, double height, std::vector<Point>& out)
{
    const QStringList values = text.toString().split(QLatin1Char(','), Qt::KeepEmptyParts);
    if (values.size() != 2)
        return;
    const auto x = parseNumber(values.at(0));
    const auto y = parseNumber(values.at(1));
    if (!x || !y)
        return;
    out.push_back(normalizedPoint(*x, *y, width, height));
}

/// Parses "x,y;x,y;..." into normalized points. Callers must discard `out`
/// unless the whole list was consumed successfully.
GeometryParseStatus
parsePointPairs(QStringView text, double width, double height, std::vector<Point>& out, std::size_t limit)
{
    // Absent geometry is valid (a metadata-only annotation); malformed content
    // is not. Whitespace alone counts as absent for symmetry with coords.
    if (text.trimmed().isEmpty())
        return GeometryParseStatus::Complete;
    const QStringList pairs = text.toString().split(QLatin1Char(';'), Qt::KeepEmptyParts);
    for (const QString& pair : pairs) {
        if (out.size() >= limit)
            return GeometryParseStatus::LimitExceeded;
        const QStringList values = pair.split(QLatin1Char(','), Qt::KeepEmptyParts);
        if (values.size() != 2)
            return GeometryParseStatus::Malformed;
        const auto x = parseNumber(values.at(0));
        const auto y = parseNumber(values.at(1));
        if (!x || !y)
            return GeometryParseStatus::Malformed;
        out.push_back(normalizedPoint(*x, *y, width, height));
    }
    return GeometryParseStatus::Complete;
}

/// Parses at most three comma-separated callout points without allocating a
/// token list proportional to the untrusted attribute size.
GeometryParseStatus parseCallout(QStringView text, double width, double height, std::vector<Point>& out)
{
    std::array<double, Limit::MaxAnnotationCalloutPoints * 2> coordinates { };
    qsizetype count = 0;
    qsizetype start = 0;
    while (start <= text.size()) {
        if (count == static_cast<qsizetype>(coordinates.size()))
            return GeometryParseStatus::LimitExceeded;
        const qsizetype comma = text.indexOf(QLatin1Char(','), start);
        const qsizetype end = comma < 0 ? text.size() : comma;
        const auto value = parseNumber(text.mid(start, end - start));
        if (!value)
            return GeometryParseStatus::Malformed;
        coordinates[static_cast<std::size_t>(count++)] = *value;
        if (comma < 0)
            break;
        start = comma + 1;
    }
    if (count < 4 || count % 2 != 0)
        return GeometryParseStatus::Malformed;

    out.reserve(static_cast<std::size_t>(count / 2));
    for (qsizetype index = 0; index < count; index += 2)
        out.push_back(normalizedPoint(coordinates[static_cast<std::size_t>(index)],
                                      coordinates[static_cast<std::size_t>(index + 1)],
                                      width,
                                      height));
    return GeometryParseStatus::Complete;
}

/// Parses a flat "x,y,x,y,..." XFDF `coords` list into quads. Each group of
/// eight numbers is top-left, top-right, bottom-left, bottom-right in
/// user-space, which the model stores as clockwise upperLeft, upperRight,
/// lowerRight, lowerLeft.
GeometryParseStatus
parseCoordList(QStringView text, double width, double height, std::vector<Quad>& out, std::size_t limit)
{
    if (text.trimmed().isEmpty())
        return GeometryParseStatus::Complete;
    const QStringList values = text.toString().split(QLatin1Char(','), Qt::KeepEmptyParts);
    qsizetype index = 0;
    while (index + 7 < values.size()) {
        if (out.size() >= limit)
            return GeometryParseStatus::LimitExceeded;
        double coordinates[8];
        for (int offset = 0; offset < 8; ++offset) {
            const auto value = parseNumber(values.at(index + offset));
            if (!value)
                return GeometryParseStatus::Malformed;
            coordinates[offset] = *value;
        }
        index += 8;
        const Point upperLeft = normalizedPoint(coordinates[0], coordinates[1], width, height);
        const Point upperRight = normalizedPoint(coordinates[2], coordinates[3], width, height);
        const Point lowerLeft = normalizedPoint(coordinates[4], coordinates[5], width, height);
        const Point lowerRight = normalizedPoint(coordinates[6], coordinates[7], width, height);
        out.push_back({ upperLeft, upperRight, lowerRight, lowerLeft });
    }
    return index == values.size() ? GeometryParseStatus::Complete : GeometryParseStatus::Malformed;
}

bool parseRect(QStringView text, double out[4])
{
    const QStringList parts = text.toString().split(QLatin1Char(','), Qt::KeepEmptyParts);
    if (parts.size() != 4)
        return false;
    for (int i = 0; i < 4; ++i) {
        const auto value = parseNumber(parts.at(i));
        if (!value)
            return false;
        out[i] = *value;
    }
    return true;
}

int parsePage(QStringView text)
{
    bool ok = false;
    const int page = text.toInt(&ok);
    return ok && page >= 0 ? page : -1;
}

std::uint8_t alphaFromOpacity(QStringView text)
{
    const auto value = parseNumber(text);
    if (!value)
        return 0xff;
    return static_cast<std::uint8_t>(std::lround(std::clamp(*value, 0.0, 1.0) * 255.0));
}

std::uint32_t composeColor(QStringView color, QStringView opacity)
{
    std::uint32_t red = 0;
    std::uint32_t green = 0;
    std::uint32_t blue = 0;
    QString value = color.toString();
    if (value.startsWith(QLatin1Char('#')))
        value.remove(0, 1);
    if (value.size() == 6) {
        bool ok = false;
        const std::uint32_t rgb = value.toUInt(&ok, 16);
        if (ok) {
            red = (rgb >> 16) & 0xff;
            green = (rgb >> 8) & 0xff;
            blue = rgb & 0xff;
        }
    }
    const std::uint32_t alpha = alphaFromOpacity(opacity);
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

int parseFlags(QStringView text)
{
    int flags = 0;
    const QStringList names = text.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& name : names) {
        if (name == QLatin1String("hidden"))
            flags |= Model::annotationFlagValue(Model::AnnotationFlag::Hidden);
        else if (name == QLatin1String("print"))
            flags |= Model::annotationFlagValue(Model::AnnotationFlag::Print);
        else if (name == QLatin1String("nozoom"))
            flags |= Model::annotationFlagValue(Model::AnnotationFlag::NoZoom);
        else if (name == QLatin1String("norotate"))
            flags |= Model::annotationFlagValue(Model::AnnotationFlag::NoRotate);
        else if (name == QLatin1String("readonly"))
            flags |= Model::annotationFlagValue(Model::AnnotationFlag::ReadOnly);
        else if (name == QLatin1String("locked"))
            flags |= Model::annotationFlagValue(Model::AnnotationFlag::Locked);
        else if (name == QLatin1String("togglenoview"))
            flags |= Model::annotationFlagValue(Model::AnnotationFlag::ToggleNoView);
        // Unknown tokens (including flags the exporter does not emit) are
        // ignored so a third-party document still imports.
    }
    return flags;
}

std::optional<AnnotationLineEnding> parseLineEnding(QStringView text)
{
    if (text == QLatin1String("None"))
        return AnnotationLineEnding::None;
    if (text == QLatin1String("Square"))
        return AnnotationLineEnding::Square;
    if (text == QLatin1String("Circle"))
        return AnnotationLineEnding::Circle;
    if (text == QLatin1String("Diamond"))
        return AnnotationLineEnding::Diamond;
    if (text == QLatin1String("OpenArrow"))
        return AnnotationLineEnding::OpenArrow;
    if (text == QLatin1String("ClosedArrow"))
        return AnnotationLineEnding::ClosedArrow;
    if (text == QLatin1String("Butt"))
        return AnnotationLineEnding::Butt;
    if (text == QLatin1String("ROpenArrow"))
        return AnnotationLineEnding::ROpenArrow;
    if (text == QLatin1String("RClosedArrow"))
        return AnnotationLineEnding::RClosedArrow;
    if (text == QLatin1String("Slash"))
        return AnnotationLineEnding::Slash;
    return std::nullopt;
}

std::optional<AnnotationIntent> parseIntent(QStringView text)
{
    if (text == QLatin1String("FreeTextCallout"))
        return AnnotationIntent::FreeTextCallout;
    if (text == QLatin1String("FreeTextTypewriter"))
        return AnnotationIntent::FreeTextTypewriter;
    if (text == QLatin1String("LineArrow"))
        return AnnotationIntent::LineArrow;
    if (text == QLatin1String("LineDimension"))
        return AnnotationIntent::LineDimension;
    if (text == QLatin1String("PolyLineDimension"))
        return AnnotationIntent::PolyLineDimension;
    if (text == QLatin1String("PolygonCloud"))
        return AnnotationIntent::PolygonCloud;
    if (text == QLatin1String("PolygonDimension"))
        return AnnotationIntent::PolygonDimension;
    if (text == QLatin1String("StampImage"))
        return AnnotationIntent::StampImage;
    if (text == QLatin1String("StampSnapshot"))
        return AnnotationIntent::StampSnapshot;
    return std::nullopt;
}

/// Parses the exporter's "D:YYYYMMDDHHmmSS+HH'mm'" form (UTC). Any other shape
/// leaves the timestamp invalid but does not reject the annotation.
Timestamp parseXfdfDate(QStringView text)
{
    Timestamp timestamp;
    const QString value = text.toString();
    if (!value.startsWith(QLatin1String("D:")) || value.size() < 16)
        return timestamp;
    const QString digits = value.mid(2, 14);
    for (const QChar ch : digits) {
        if (!ch.isDigit())
            return timestamp;
    }
    const QDate date(digits.mid(0, 4).toInt(), digits.mid(4, 2).toInt(), digits.mid(6, 2).toInt());
    const QTime time(digits.mid(8, 2).toInt(), digits.mid(10, 2).toInt(), digits.mid(12, 2).toInt());
    if (!date.isValid() || !time.isValid())
        return timestamp;
    timestamp.valid = true;
    timestamp.unixMilliseconds = QDateTime(date, time, QTimeZone::UTC).toMSecsSinceEpoch();
    return timestamp;
}

/// Reverses the exporter's PDF-name escaping: "#XX" byte escapes are decoded
/// and the result is read back as UTF-8.
QString decodePdfName(QStringView text)
{
    QByteArray bytes;
    const QString value = text.toString();
    for (int i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);
        if (ch == QLatin1Char('#') && i + 2 < value.size()) {
            bool ok = false;
            const uint byte = value.mid(i + 1, 2).toUInt(&ok, 16);
            if (ok) {
                bytes.append(static_cast<char>(byte));
                i += 2;
                continue;
            }
        }
        if (ch == QLatin1Char('/') && bytes.isEmpty())
            continue; // Drop the PDF name introducer.
        bytes.append(ch.toLatin1());
    }
    return QString::fromUtf8(bytes);
}

void applyDefaultAppearance(const QString& text, Annotation& annotation)
{
    const QStringList tokens = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.size() < 2 || tokens.back() != QLatin1String("Tf"))
        return;
    const auto size = parseNumber(tokens.at(tokens.size() - 2));
    if (!size)
        return;
    QString fontName;
    if (tokens.size() >= 3)
        fontName = decodePdfName(tokens.at(tokens.size() - 3));
    if (!annotation.extras.style.appearance)
        annotation.extras.style.appearance = AnnotationAppearance { };
    annotation.extras.style.appearance->fontName = fontName.toStdString();
    annotation.extras.style.appearance->fontSize = *size;
}

void ensureAppearance(Annotation& annotation)
{
    if (!annotation.extras.style.appearance)
        annotation.extras.style.appearance = AnnotationAppearance { };
}

std::optional<AnnotationType> subtypeForElement(QStringView name)
{
    if (name == QLatin1String("text"))
        return AnnotationType::Text;
    if (name == QLatin1String("freetext"))
        return AnnotationType::FreeText;
    if (name == QLatin1String("line"))
        return AnnotationType::Line;
    if (name == QLatin1String("polygon"))
        return AnnotationType::Polygon;
    if (name == QLatin1String("polyline"))
        return AnnotationType::PolyLine;
    if (name == QLatin1String("square"))
        return AnnotationType::Square;
    if (name == QLatin1String("circle"))
        return AnnotationType::Circle;
    if (name == QLatin1String("highlight"))
        return AnnotationType::Highlight;
    if (name == QLatin1String("underline"))
        return AnnotationType::Underline;
    if (name == QLatin1String("squiggly"))
        return AnnotationType::Squiggly;
    if (name == QLatin1String("strikeout"))
        return AnnotationType::StrikeOut;
    if (name == QLatin1String("ink"))
        return AnnotationType::Ink;
    if (name == QLatin1String("stamp"))
        return AnnotationType::Stamp;
    if (name == QLatin1String("caret"))
        return AnnotationType::Caret;
    return std::nullopt;
}

/// Tightens the annotation rectangle to its parsed geometry so re-exporting
/// reproduces the same tight `rect` the exporter derives from geometry.
void tightenBounds(Annotation& annotation)
{
    bool valid = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const auto add = [&](const Point& point) {
        if (!valid) {
            x0 = x1 = point.x;
            y0 = y1 = point.y;
            valid = true;
            return;
        }
        x0 = std::min(x0, point.x);
        y0 = std::min(y0, point.y);
        x1 = std::max(x1, point.x);
        y1 = std::max(y1, point.y);
    };
    for (const Point& point : annotation.extras.points)
        add(point);
    for (const Quad& quad : annotation.extras.quads) {
        add(quad.upperLeft);
        add(quad.upperRight);
        add(quad.lowerRight);
        add(quad.lowerLeft);
    }
    for (const auto& path : annotation.extras.inkPaths) {
        for (const Point& point : path)
            add(point);
    }
    if (valid) {
        annotation.x0 = x0;
        annotation.y0 = y0;
        annotation.x1 = x1;
        annotation.y1 = y1;
    }
}

struct Entry {
    int page = -1;
    QString rect, color, opacity, title, name, creationDate, date, flags;
    QString contents, defaultAppearance, vertices, coords;
    QString icon, intent, width, start, end, head, tail, symbol, callout;
    QStringList inkGestures;
    bool inkPathLimitExceeded = false;
};

class Importer {
public:
    explicit Importer(const QVector<QSizeF>& pageSizes, const XfdfParseLimits& limits)
        : m_pageSizes(pageSizes)
        , m_annotationsRemaining(std::min(limits.annotationsRemaining, Limit::MaxAnnotationsPerDocument))
    {
        m_annotationsPerPageRemaining.reserve(pageSizes.size());
        if (limits.annotationsPerPageRemaining.isEmpty()) {
            for (qsizetype i = 0; i < pageSizes.size(); ++i)
                m_annotationsPerPageRemaining.append(Limit::MaxAnnotationsPerPage);
        } else if (limits.annotationsPerPageRemaining.size() == pageSizes.size()) {
            for (const std::size_t remaining : limits.annotationsPerPageRemaining)
                m_annotationsPerPageRemaining.append(std::min(remaining, Limit::MaxAnnotationsPerPage));
        } else {
            m_limitsError = QStringLiteral("per-page annotation limits do not match the page count");
        }
    }

    XfdfParseResult run(const QByteArray& xml, QString* error)
    {
        if (error)
            error->clear();
        m_result.pages.resize(m_pageSizes.size());
        for (int i = 0; i < m_pageSizes.size(); ++i) {
            m_result.pages[i].widthPoints = m_pageSizes.at(i).width();
            m_result.pages[i].heightPoints = m_pageSizes.at(i).height();
        }
        if (!m_limitsError.isEmpty())
            return fail(error, m_limitsError);
        if (xml.isEmpty())
            return fail(error, QStringLiteral("empty XFDF input"));
        if (xml.size() > MaxXfdfBytes)
            return fail(error, QStringLiteral("XFDF input exceeds the size limit"));

        QXmlStreamReader reader(xml);
        bool sawAnnots = false;
        while (!reader.atEnd()) {
            const QXmlStreamReader::TokenType token = reader.readNext();
            if (token == QXmlStreamReader::Invalid)
                return fail(error, reader.errorString());
            if (token == QXmlStreamReader::DTD)
                return fail(error, QStringLiteral("DOCTYPE is not allowed in XFDF"));
            if (token != QXmlStreamReader::StartElement)
                continue;
            if (reader.name() == QLatin1String("annots")) {
                sawAnnots = true;
                parseAnnots(reader);
                if (!m_limitsError.isEmpty())
                    return fail(error, m_limitsError);
                if (reader.hasError())
                    return fail(error, reader.errorString());
            }
        }
        if (reader.hasError())
            return fail(error, reader.errorString());
        if (!sawAnnots)
            return fail(error, QStringLiteral("missing <annots> element"));
        flushWarnings();
        return std::move(m_result);
    }

private:
    XfdfParseResult fail(QString* error, const QString& message)
    {
        if (error)
            *error = message;
        m_result.pages.clear();
        return std::move(m_result);
    }

    void warn(const QString& message)
    {
        if (m_result.warnings.size() < MaxWarnings)
            m_result.warnings.append(message);
        else
            ++m_suppressed;
    }

    void skip(const QString& message)
    {
        ++m_result.skipped;
        warn(message);
    }

    void flushWarnings()
    {
        if (m_suppressed > 0)
            m_result.warnings.append(QStringLiteral("...and %1 more").arg(m_suppressed));
    }

    void parseAnnots(QXmlStreamReader& reader)
    {
        while (!reader.atEnd()) {
            const QXmlStreamReader::TokenType token = reader.readNext();
            if (token == QXmlStreamReader::Invalid)
                return;
            if (token == QXmlStreamReader::EndElement && reader.name() == QLatin1String("annots"))
                return;
            if (token != QXmlStreamReader::StartElement)
                continue;
            const QStringView name = reader.name();
            const auto subtype = subtypeForElement(name);
            if (!subtype) {
                reader.skipCurrentElement();
                skip(QStringLiteral("unknown element <%1>").arg(name.toString()));
                continue;
            }
            parseAnnotation(reader, *subtype);
            if (!m_limitsError.isEmpty())
                return;
        }
    }

    void parseAnnotation(QXmlStreamReader& reader, AnnotationType subtype)
    {
        const QXmlStreamAttributes attributes = reader.attributes();
        Entry entry;
        entry.page = parsePage(attributes.value(QLatin1String("page")));
        entry.rect = attributes.value(QLatin1String("rect")).toString();
        entry.color = attributes.value(QLatin1String("color")).toString();
        entry.opacity = attributes.value(QLatin1String("opacity")).toString();
        entry.title = attributes.value(QLatin1String("title")).toString();
        entry.name = attributes.value(QLatin1String("name")).toString();
        entry.creationDate = attributes.value(QLatin1String("creationdate")).toString();
        entry.date = attributes.value(QLatin1String("date")).toString();
        entry.flags = attributes.value(QLatin1String("flags")).toString();
        entry.icon = attributes.value(QLatin1String("icon")).toString();
        entry.intent = attributes.value(QLatin1String("intent")).toString();
        entry.width = attributes.value(QLatin1String("width")).toString();
        entry.coords = attributes.value(QLatin1String("coords")).toString();
        entry.start = attributes.value(QLatin1String("start")).toString();
        entry.end = attributes.value(QLatin1String("end")).toString();
        entry.head = attributes.value(QLatin1String("head")).toString();
        entry.tail = attributes.value(QLatin1String("tail")).toString();
        entry.symbol = attributes.value(QLatin1String("symbol")).toString();
        entry.callout = attributes.value(QLatin1String("callout")).toString();

        while (!reader.atEnd()) {
            const QXmlStreamReader::TokenType token = reader.readNext();
            if (token == QXmlStreamReader::Invalid || token == QXmlStreamReader::EndElement)
                break;
            if (token != QXmlStreamReader::StartElement)
                continue;
            const QStringView child = reader.name();
            if (child == QLatin1String("contents"))
                entry.contents = reader.readElementText(QXmlStreamReader::IncludeChildElements);
            else if (child == QLatin1String("defaultappearance"))
                entry.defaultAppearance = reader.readElementText(QXmlStreamReader::IncludeChildElements);
            else if (child == QLatin1String("vertices"))
                entry.vertices = reader.readElementText(QXmlStreamReader::IncludeChildElements);
            else if (child == QLatin1String("inklist"))
                parseInkList(reader, entry);
            else
                reader.skipCurrentElement();
        }
        finalize(entry, subtype);
    }

    void parseInkList(QXmlStreamReader& reader, Entry& entry)
    {
        while (!reader.atEnd()) {
            const QXmlStreamReader::TokenType token = reader.readNext();
            if (token == QXmlStreamReader::Invalid)
                return;
            if (token == QXmlStreamReader::EndElement && reader.name() == QLatin1String("inklist"))
                return;
            if (token != QXmlStreamReader::StartElement)
                continue;
            if (reader.name() == QLatin1String("gesture")) {
                if (entry.inkGestures.size() >= static_cast<qsizetype>(Limit::MaxAnnotationInkPaths)) {
                    entry.inkPathLimitExceeded = true;
                    reader.skipCurrentElement();
                } else {
                    entry.inkGestures.append(reader.readElementText(QXmlStreamReader::IncludeChildElements));
                }
            } else {
                reader.skipCurrentElement();
            }
        }
    }

    bool applyGeometry(const Entry& entry, AnnotationType subtype, double width, double height, Annotation& annotation)
    {
        const auto rejectGeometry = [&](GeometryParseStatus status, const QString& label) {
            skip(status == GeometryParseStatus::LimitExceeded
                     ? QStringLiteral("%1 geometry exceeds its limit on page %2").arg(label).arg(entry.page)
                     : QStringLiteral("malformed %1 geometry on page %2").arg(label).arg(entry.page));
            return false;
        };
        if (const auto border = parseNumber(entry.width); border && *border >= 0)
            annotation.extras.style.borderWidth = *border;
        if (const auto intent = parseIntent(entry.intent))
            annotation.extras.style.intent = *intent;

        switch (subtype) {
        case AnnotationType::Text:
        case AnnotationType::FreeText:
            if (!entry.icon.isEmpty()) {
                ensureAppearance(annotation);
                annotation.extras.style.appearance->icon = entry.icon.toStdString();
            }
            if (!entry.defaultAppearance.isEmpty())
                applyDefaultAppearance(entry.defaultAppearance, annotation);
            if (!entry.callout.trimmed().isEmpty()) {
                std::vector<Point> callout;
                const GeometryParseStatus status = parseCallout(entry.callout, width, height, callout);
                if (status != GeometryParseStatus::Complete)
                    return rejectGeometry(status, QStringLiteral("callout"));
                if (!entry.intent.trimmed().isEmpty()
                    && annotation.extras.style.intent != AnnotationIntent::FreeTextCallout)
                    return rejectGeometry(GeometryParseStatus::Malformed, QStringLiteral("callout intent"));
                annotation.extras.callout = std::move(callout);
                if (!annotation.extras.style.intent)
                    annotation.extras.style.intent = AnnotationIntent::FreeTextCallout;
            }
            break;
        case AnnotationType::Line: {
            std::vector<Point> points;
            parseSinglePoint(entry.start, width, height, points);
            parseSinglePoint(entry.end, width, height, points);
            // Unlike the branches below, endpoint-less lines stay rejected:
            // the exporter cannot re-emit them, so accepting would only move
            // the asymmetry.
            if (points.size() < 2) {
                skip(QStringLiteral("line on page %1 has fewer than two endpoints").arg(entry.page));
                return false;
            }
            annotation.extras.points = std::move(points);
            if (const auto ending = parseLineEnding(entry.tail))
                annotation.extras.style.firstLineEnding = *ending;
            if (const auto ending = parseLineEnding(entry.head))
                annotation.extras.style.lastLineEnding = *ending;
            break;
        }
        case AnnotationType::Polygon:
        case AnnotationType::PolyLine: {
            // Empty vertices are accepted: the exporter re-emits them as an
            // empty <vertices> element, so the round trip is stable.
            std::vector<Point> points;
            const GeometryParseStatus status =
                parsePointPairs(entry.vertices, width, height, points, Limit::MaxAnnotationPoints);
            if (status != GeometryParseStatus::Complete)
                return rejectGeometry(status, QStringLiteral("polygon"));
            annotation.extras.points = std::move(points);
            break;
        }
        case AnnotationType::Highlight:
        case AnnotationType::Underline:
        case AnnotationType::Squiggly:
        case AnnotationType::StrikeOut: {
            // Quad-less markup is accepted: the exporter re-emits the element
            // without a coords attribute, so the round trip is stable.
            std::vector<Quad> quads;
            const GeometryParseStatus status =
                parseCoordList(entry.coords, width, height, quads, Limit::MaxAnnotationQuads);
            if (status != GeometryParseStatus::Complete)
                return rejectGeometry(status, QStringLiteral("highlight"));
            annotation.extras.quads = std::move(quads);
            break;
        }
        case AnnotationType::Ink: {
            if (entry.inkPathLimitExceeded)
                return rejectGeometry(GeometryParseStatus::LimitExceeded, QStringLiteral("ink"));
            // A missing ink list is accepted (a metadata-only annotation the
            // exporter re-emits without <inklist>). Empty gestures are dropped
            // rather than passed on, keeping the worker's ink call safely fed.
            std::size_t remaining = Limit::MaxAnnotationInkPoints;
            for (const QString& gesture : entry.inkGestures) {
                std::vector<Point> path;
                const GeometryParseStatus status = parsePointPairs(gesture, width, height, path, remaining);
                if (status != GeometryParseStatus::Complete)
                    return rejectGeometry(status, QStringLiteral("ink"));
                if (path.empty())
                    continue;
                remaining -= path.size();
                annotation.extras.inkPaths.push_back(std::move(path));
            }
            break;
        }
        case AnnotationType::Stamp:
            if (!entry.icon.isEmpty()) {
                ensureAppearance(annotation);
                annotation.extras.style.appearance->icon = entry.icon.toStdString();
            }
            break;
        case AnnotationType::Caret:
            annotation.extras.caretSymbolP = entry.symbol == QLatin1String("P");
            break;
        default:
            break;
        }
        tightenBounds(annotation);
        return true;
    }

    void finalize(const Entry& entry, AnnotationType subtype)
    {
        if (entry.page < 0 || entry.page >= m_pageSizes.size()) {
            skip(QStringLiteral("annotation references out-of-range page %1").arg(entry.page));
            return;
        }
        const QSizeF size = m_pageSizes.at(entry.page);
        const double width = size.width();
        const double height = size.height();
        if (!(width > 0) || !(height > 0)) {
            skip(QStringLiteral("page %1 has invalid geometry").arg(entry.page));
            return;
        }
        double rect[4];
        if (!parseRect(entry.rect, rect)) {
            skip(QStringLiteral("annotation on page %1 has an invalid rect").arg(entry.page));
            return;
        }

        Annotation annotation;
        annotation.subtype = subtype;
        annotation.uuid = entry.name.toStdString();
        annotation.contents = entry.contents.toStdString();
        annotation.author = entry.title.toStdString();
        annotation.creationDate = parseXfdfDate(entry.creationDate);
        annotation.modificationDate = parseXfdfDate(entry.date);
        annotation.color = composeColor(entry.color, entry.opacity);
        annotation.flags = parseFlags(entry.flags);
        // rect is "left,bottom,right,top" in user-space; the shared conversion
        // flips Y into the model's top-left downward frame. Normalizing the two
        // diagonal corners preserves the ordering guarantees of the input rect.
        const Point topLeft =
            Model::userSpaceToNormalized({ std::min(rect[0], rect[2]), std::max(rect[1], rect[3]) }, width, height);
        const Point bottomRight =
            Model::userSpaceToNormalized({ std::max(rect[0], rect[2]), std::min(rect[1], rect[3]) }, width, height);
        annotation.x0 = topLeft.x;
        annotation.y0 = topLeft.y;
        annotation.x1 = bottomRight.x;
        annotation.y1 = bottomRight.y;

        if (!applyGeometry(entry, subtype, width, height, annotation))
            return;

        if (static_cast<std::size_t>(m_result.applied) >= m_annotationsRemaining
            || static_cast<std::size_t>(m_result.pages.at(entry.page).annotations.size())
                >= m_annotationsPerPageRemaining.at(entry.page)) {
            m_limitsError = QStringLiteral("XFDF annotations exceed the target PDF's remaining capacity");
            return;
        }

        m_result.pages[entry.page].annotations.append(std::move(annotation));
        ++m_result.applied;
    }

    QVector<QSizeF> m_pageSizes;
    QVector<std::size_t> m_annotationsPerPageRemaining;
    std::size_t m_annotationsRemaining = 0;
    QString m_limitsError;
    XfdfParseResult m_result;
    int m_suppressed = 0;
};

} // namespace

XfdfParseResult xfdfToAnnotations(const QByteArray& xml,
                                  const QVector<QSizeF>& pageSizes,
                                  QString* error,
                                  const XfdfParseLimits& limits)
{
    Importer importer(pageSizes, limits);
    return importer.run(xml, error);
}

} // namespace Mu::Plugin::Xfdf
