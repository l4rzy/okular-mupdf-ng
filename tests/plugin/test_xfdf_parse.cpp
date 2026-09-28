// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVector>

#include "plugin/xfdf/export.hpp"
#include "plugin/xfdf/import.hpp"
#include "shared/protocol/limits.hpp"

#include <cmath>

using Mu::Model::Annotation;
using Mu::Model::AnnotationLineEnding;
using Mu::Model::AnnotationType;
using Mu::Plugin::Xfdf::Page;
using Mu::Plugin::Xfdf::XfdfParseResult;

namespace {

const QVector<QSizeF> kPageSize { QSizeF(200, 100) };

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

QString document(const QString& annots)
{
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n<annots>\n")
        + annots + QStringLiteral("\n</annots>\n</xfdf>\n");
}

XfdfParseResult parse(const QString& xml, const QVector<QSizeF>& sizes = kPageSize)
{
    QString error;
    XfdfParseResult result = Mu::Plugin::Xfdf::xfdfToAnnotations(xml.toUtf8(), sizes, &error);
    Q_ASSERT(error.isEmpty());
    return result;
}

} // namespace

class TestPluginXfdfParse : public QObject {
    Q_OBJECT

private slots:

    void parsesTextAnnotation()
    {
        const XfdfParseResult result = parse(document(QStringLiteral(
            "<text page=\"0\" rect=\"20,40,100,80\" color=\"#ff0000\" opacity=\"1\" title=\"alice\" name=\"u1\">"
            "<contents>hello</contents></text>")));

        QCOMPARE(result.pages.size(), 1);
        QCOMPARE(result.applied, 1);
        QCOMPARE(result.skipped, 0);
        QCOMPARE(result.pages.at(0).widthPoints, 200.0);
        QCOMPARE(result.pages.at(0).annotations.size(), 1);

        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::Text);
        QCOMPARE(annotation.uuid, std::string("u1"));
        QCOMPARE(annotation.author, std::string("alice"));
        QCOMPARE(annotation.contents, std::string("hello"));
        QCOMPARE(annotation.color, 0xffff0000U);
        QVERIFY(near(annotation.x0, 0.1));
        QVERIFY(near(annotation.y0, 0.2));
        QVERIFY(near(annotation.x1, 0.5));
        QVERIFY(near(annotation.y1, 0.6));
    }

    void parsesHighlightQuadOrder()
    {
        const XfdfParseResult result =
            parse(document(QStringLiteral("<highlight page=\"0\" rect=\"20,70,100,80\" color=\"#ffff00\" opacity=\"1\" "
                                          "coords=\"20,80,100,80,20,70,100,70\"/>")));

        QCOMPARE(result.applied, 1);
        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::Highlight);
        QCOMPARE(annotation.extras.quads.size(), std::size_t(1));
        const Mu::Model::Quad& quad = annotation.extras.quads.front();
        // XFDF coords are top-left, top-right, bottom-left, bottom-right.
        QVERIFY(near(quad.upperLeft.x, 0.1) && near(quad.upperLeft.y, 0.2));
        QVERIFY(near(quad.upperRight.x, 0.5) && near(quad.upperRight.y, 0.2));
        QVERIFY(near(quad.lowerRight.x, 0.5) && near(quad.lowerRight.y, 0.3));
        QVERIFY(near(quad.lowerLeft.x, 0.1) && near(quad.lowerLeft.y, 0.3));
    }

    void parsesLineEndpointsAndEndings()
    {
        const XfdfParseResult result =
            parse(document(QStringLiteral("<line page=\"0\" rect=\"20,80,40,90\" color=\"#000000\" opacity=\"1\" "
                                          "start=\"20,90\" end=\"40,80\" tail=\"Square\" head=\"ClosedArrow\"/>")));

        QCOMPARE(result.applied, 1);
        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::Line);
        QCOMPARE(annotation.extras.points.size(), std::size_t(2));
        QVERIFY(near(annotation.extras.points.front().x, 0.1));
        QVERIFY(near(annotation.extras.points.front().y, 0.1));
        QVERIFY(near(annotation.extras.points.back().x, 0.2));
        QVERIFY(near(annotation.extras.points.back().y, 0.2));
        QCOMPARE(*annotation.extras.style.firstLineEnding, AnnotationLineEnding::Square);
        QCOMPARE(*annotation.extras.style.lastLineEnding, AnnotationLineEnding::ClosedArrow);
    }

    void parsesPolylineVertices()
    {
        const XfdfParseResult result =
            parse(document(QStringLiteral("<polyline page=\"0\" rect=\"20,70,60,90\" color=\"#000000\" opacity=\"1\">"
                                          "<vertices>20,90;40,80;60,70</vertices></polyline>")));

        QCOMPARE(result.applied, 1);
        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::PolyLine);
        QCOMPARE(annotation.extras.points.size(), std::size_t(3));
        QVERIFY(near(annotation.extras.points.at(2).x, 0.3));
        QVERIFY(near(annotation.extras.points.at(2).y, 0.3));
    }

    void parsesInkGestures()
    {
        const XfdfParseResult result = parse(document(
            QStringLiteral("<ink page=\"0\" rect=\"20,60,60,90\" color=\"#000000\" opacity=\"1\">"
                           "<inklist><gesture>20,90;40,80</gesture><gesture>40,70;60,60</gesture></inklist></ink>")));

        QCOMPARE(result.applied, 1);
        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::Ink);
        QCOMPARE(annotation.extras.inkPaths.size(), std::size_t(2));
        QCOMPARE(annotation.extras.inkPaths.at(0).size(), std::size_t(2));
        QCOMPARE(annotation.extras.inkPaths.at(1).size(), std::size_t(2));
    }

    void parsesFreeTextDefaultAppearance()
    {
        const XfdfParseResult result = parse(document(
            QStringLiteral("<freetext page=\"0\" rect=\"20,40,100,80\" color=\"#000000\" opacity=\"1\">"
                           "<contents>note</contents>"
                           "<defaultappearance>0 0 0 rg /DejaVu#20Sans 12 Tf</defaultappearance></freetext>")));

        QCOMPARE(result.applied, 1);
        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::FreeText);
        QVERIFY(annotation.extras.style.appearance.has_value());
        QCOMPARE(annotation.extras.style.appearance->fontName, std::string("DejaVu Sans"));
        QVERIFY(near(annotation.extras.style.appearance->fontSize, 12.0));
    }

    void parsesColorOpacityFlagsAndDates()
    {
        const XfdfParseResult result = parse(document(QStringLiteral(
            "<text page=\"0\" rect=\"20,40,100,80\" color=\"#00ff00\" opacity=\"0.5\" "
            "flags=\"hidden,print\" creationdate=\"D:19691231235959+00'00'\" date=\"D:19700101000000+00'00'\"/>")));

        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.color, 0x8000ff00U);
        QCOMPARE(annotation.flags, 6);
        QVERIFY(annotation.creationDate.valid);
        QCOMPARE(annotation.creationDate.unixMilliseconds, std::int64_t(-1000));
        QVERIFY(annotation.modificationDate.valid);
        QCOMPARE(annotation.modificationDate.unixMilliseconds, std::int64_t(0));
    }

    void skipsUnknownElement()
    {
        const XfdfParseResult result = parse(document(QStringLiteral("<sound page=\"0\" rect=\"0,0,1,1\"/>")));
        QCOMPARE(result.applied, 0);
        QCOMPARE(result.skipped, 1);
        QVERIFY(!result.warnings.isEmpty());
    }

    void skipsOutOfRangePage()
    {
        const XfdfParseResult result = parse(document(QStringLiteral("<text page=\"7\" rect=\"20,40,100,80\"/>")));
        QCOMPARE(result.applied, 0);
        QCOMPARE(result.skipped, 1);
    }

    void skipsInvalidRect()
    {
        const XfdfParseResult result = parse(document(QStringLiteral("<text page=\"0\" rect=\"20,40,100\"/>")));
        QCOMPARE(result.applied, 0);
        QCOMPARE(result.skipped, 1);
    }

    void nonFiniteCoordinatesAreClamped()
    {
        const XfdfParseResult result =
            parse(document(QStringLiteral("<text page=\"0\" rect=\"nan,0,inf,80\" color=\"#000000\" opacity=\"1\"/>")));
        QCOMPARE(result.applied, 0);
        QCOMPARE(result.skipped, 1);
    }

    void rejectsMalformedXml()
    {
        QString error;
        const XfdfParseResult result = Mu::Plugin::Xfdf::xfdfToAnnotations(
            QByteArrayLiteral("<xfdf><annots><text page=\"0\"></annots></xfdf>"), kPageSize, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(result.pages.isEmpty());
    }

    void rejectsDoctype()
    {
        QString error;
        const QByteArray xml =
            QByteArrayLiteral("<?xml version=\"1.0\"?><!DOCTYPE xfdf [<!ENTITY e \"boom\">]><xfdf><annots/></xfdf>");
        const XfdfParseResult result = Mu::Plugin::Xfdf::xfdfToAnnotations(xml, kPageSize, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(result.pages.isEmpty());
    }

    void missingAnnotsIsError()
    {
        QString error;
        const XfdfParseResult result =
            Mu::Plugin::Xfdf::xfdfToAnnotations(QByteArrayLiteral("<xfdf></xfdf>"), kPageSize, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(result.pages.isEmpty());
    }

    void rejectsExcessVertices()
    {
        QStringList pairs;
        for (int i = 0; i < 20'001; ++i)
            pairs.append(QStringLiteral("20,90"));
        const XfdfParseResult result = parse(document(
            QStringLiteral("<polygon page=\"0\" rect=\"20,70,60,90\" color=\"#000000\" opacity=\"1\"><vertices>")
            + pairs.join(QLatin1Char(';')) + QStringLiteral("</vertices></polygon>")));

        QCOMPARE(result.applied, 0);
        QCOMPARE(result.skipped, 1);
        QVERIFY(result.pages.at(0).annotations.isEmpty());
        QVERIFY(!result.warnings.isEmpty());
    }

    void acceptsHighlightWithoutCoords()
    {
        const XfdfParseResult result = parse(document(QStringLiteral(
            "<highlight page=\"0\" rect=\"20,70,100,80\" color=\"#ffff00\" opacity=\"1\" title=\"note\"/>")));

        QCOMPARE(result.applied, 1);
        QCOMPARE(result.skipped, 0);
        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::Highlight);
        QVERIFY(annotation.extras.quads.empty());
        QCOMPARE(annotation.author, std::string("note"));
        QVERIFY(near(annotation.x0, 0.1));
        QVERIFY(near(annotation.y0, 0.2));
        QVERIFY(near(annotation.x1, 0.5));
        QVERIFY(near(annotation.y1, 0.3));

        // The exporter re-emits the quad-less element, so the round trip is stable.
        const QString roundTrip = Mu::Plugin::Xfdf::annotationsToXfdf(result.pages);
        QVERIFY(roundTrip.contains(QStringLiteral("<highlight")));
        QVERIFY(!roundTrip.contains(QStringLiteral("coords=")));
    }

    void acceptsPolygonWithEmptyVertices()
    {
        const XfdfParseResult result =
            parse(document(QStringLiteral("<polygon page=\"0\" rect=\"20,70,60,90\" color=\"#000000\" opacity=\"1\">"
                                          "<vertices></vertices></polygon>")));

        QCOMPARE(result.applied, 1);
        QCOMPARE(result.skipped, 0);
        QCOMPARE(result.pages.at(0).annotations.front().extras.points.size(), std::size_t(0));
    }

    void acceptsInkWithoutGesturesAndDropsEmptyOnes()
    {
        const XfdfParseResult bare =
            parse(document(QStringLiteral("<ink page=\"0\" rect=\"20,60,60,90\" color=\"#000000\" opacity=\"1\"/>")));
        QCOMPARE(bare.applied, 1);
        QCOMPARE(bare.skipped, 0);
        QVERIFY(bare.pages.at(0).annotations.front().extras.inkPaths.empty());

        const XfdfParseResult mixed = parse(
            document(QStringLiteral("<ink page=\"0\" rect=\"20,60,60,90\" color=\"#000000\" opacity=\"1\">"
                                    "<inklist><gesture></gesture><gesture>20,90;40,80</gesture></inklist></ink>")));
        QCOMPARE(mixed.applied, 1);
        QCOMPARE(mixed.pages.at(0).annotations.front().extras.inkPaths.size(), std::size_t(1));
    }

    void rejectsLineWithoutEndpoints()
    {
        // Endpoint-less lines stay rejected: the exporter cannot re-emit them.
        const XfdfParseResult result =
            parse(document(QStringLiteral("<line page=\"0\" rect=\"20,80,40,90\" color=\"#000000\" opacity=\"1\"/>")));
        QCOMPARE(result.applied, 0);
        QCOMPARE(result.skipped, 1);
    }

    void roundTripsGeometryTypesThroughExporter()
    {
        Page source;
        source.widthPoints = 200;
        source.heightPoints = 100;

        Annotation highlight;
        highlight.subtype = AnnotationType::Highlight;
        highlight.x0 = 0.1;
        highlight.y0 = 0.2;
        highlight.x1 = 0.5;
        highlight.y1 = 0.3;
        highlight.color = 0xffffff00U;
        highlight.extras.quads = { { { 0.1, 0.2 }, { 0.5, 0.2 }, { 0.5, 0.3 }, { 0.1, 0.3 } } };
        source.annotations.append(highlight);

        Annotation line;
        line.subtype = AnnotationType::Line;
        line.x0 = 0.1;
        line.y0 = 0.1;
        line.x1 = 0.2;
        line.y1 = 0.2;
        line.color = 0xff000000U;
        line.extras.points = { { 0.1, 0.1 }, { 0.2, 0.2 } };
        line.extras.style.firstLineEnding = AnnotationLineEnding::Square;
        line.extras.style.lastLineEnding = AnnotationLineEnding::ClosedArrow;
        source.annotations.append(line);

        Annotation ink;
        ink.subtype = AnnotationType::Ink;
        ink.x0 = 0.1;
        ink.y0 = 0.1;
        ink.x1 = 0.3;
        ink.y1 = 0.4;
        ink.color = 0xff000000U;
        ink.extras.inkPaths = { { { 0.1, 0.1 }, { 0.2, 0.2 } }, { { 0.2, 0.3 }, { 0.3, 0.4 } } };
        source.annotations.append(ink);

        const QString xml = Mu::Plugin::Xfdf::annotationsToXfdf({ source });
        const XfdfParseResult result = parse(xml);

        QCOMPARE(result.applied, 3);
        QCOMPARE(result.skipped, 0);

        const auto& highlightRound = result.pages.at(0).annotations.at(0);
        QCOMPARE(highlightRound.subtype, AnnotationType::Highlight);
        QVERIFY(near(highlightRound.extras.quads.front().upperLeft.x, 0.1));
        QVERIFY(near(highlightRound.extras.quads.front().lowerRight.y, 0.3));

        const auto& lineRound = result.pages.at(0).annotations.at(1);
        QCOMPARE(lineRound.subtype, AnnotationType::Line);
        QCOMPARE(*lineRound.extras.style.firstLineEnding, AnnotationLineEnding::Square);
        QCOMPARE(*lineRound.extras.style.lastLineEnding, AnnotationLineEnding::ClosedArrow);

        const auto& inkRound = result.pages.at(0).annotations.at(2);
        QCOMPARE(inkRound.subtype, AnnotationType::Ink);
        QCOMPARE(inkRound.extras.inkPaths.size(), std::size_t(2));
    }

    void roundTripsTextMetadataThroughExporter()
    {
        Page source;
        source.widthPoints = 200;
        source.heightPoints = 100;
        Annotation text;
        text.subtype = AnnotationType::Text;
        text.x0 = 0.1;
        text.y0 = 0.2;
        text.x1 = 0.5;
        text.y1 = 0.6;
        text.color = 0xffff0000U;
        text.contents = "hello <world> & \"friends\"";
        text.author = "bob";
        text.uuid = "u1";
        text.flags = 6;
        text.creationDate = { true, -1000 };
        text.modificationDate = { true, 0 };
        source.annotations.append(text);

        const QString xml = Mu::Plugin::Xfdf::annotationsToXfdf({ source });
        const XfdfParseResult result = parse(xml);

        QCOMPARE(result.applied, 1);
        const Annotation& annotation = result.pages.at(0).annotations.front();
        QCOMPARE(annotation.subtype, AnnotationType::Text);
        QCOMPARE(annotation.contents, text.contents);
        QCOMPARE(annotation.author, text.author);
        QCOMPARE(annotation.uuid, text.uuid);
        QCOMPARE(annotation.flags, text.flags);
        QCOMPARE(annotation.color, text.color);
        QVERIFY(near(annotation.x0, text.x0));
        QVERIFY(near(annotation.y0, text.y0));
        QVERIFY(near(annotation.x1, text.x1));
        QVERIFY(near(annotation.y1, text.y1));
        QCOMPARE(annotation.creationDate.unixMilliseconds, text.creationDate.unixMilliseconds);
    }
};

QTEST_MAIN(TestPluginXfdfParse)
#include "test_xfdf_parse.moc"
