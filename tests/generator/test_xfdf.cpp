// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/xfdf.hpp"
#include "plugin/xfdf/export.hpp"
#include "plugin/xfdf/import.hpp"

#include <QDateTime>
#include <QFont>
#include <QTest>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <okular/core/annotations.h>
#include <okular/core/page.h>

#include <cmath>
#include <limits>
#include <string>

using Mu::Generator::Conversion::annotationsToXfdf;
using Mu::Plugin::Xfdf::normalizedRectToUserSpace;
using Mu::Plugin::Xfdf::Page;

namespace Xfdf = Mu::Plugin::Xfdf;
using Mu::Model::Annotation;

namespace {

/// Builds a 200x100 page (user-space points).
Okular::Page* makePage(double width = 200, double height = 100)
{
    // Okular::Page takes ownership of annotations added to it, so callers only
    // own the returned page.
    return new Okular::Page(1, width, height, Okular::Rotation0);
}

/// Serializes via the generator overload at 72 dpi, where device pixels equal
/// PDF points, so page dimensions pass through unscaled.
QString generatorXfdf(std::initializer_list<Okular::Page*> pages)
{
    return annotationsToXfdf(QVector<Okular::Page*>(pages), QSizeF(72, 72));
}

Annotation baseAnnotation(Mu::Model::AnnotationType type, double x0, double y0, double x1, double y1)
{
    Annotation annotation;
    annotation.subtype = type;
    annotation.x0 = x0;
    annotation.y0 = y0;
    annotation.x1 = x1;
    annotation.y1 = y1;
    annotation.color = 0xffff0000U; // opaque red
    annotation.contents = "note";
    annotation.author = "alice";
    annotation.uuid = "uuid-1";
    return annotation;
}

} // namespace

class TestGeneratorXfdf : public QObject {
    Q_OBJECT

private slots:

    void emptyDocumentHasEmptyAnnots()
    {
        const QString xfdf = generatorXfdf({ });
        QVERIFY(xfdf.contains(QStringLiteral("<annots>")));
        QVERIFY(xfdf.contains(QStringLiteral("</annots>")));
        QVERIFY(xfdf.contains(QStringLiteral("http://ns.adobe.com/xfdf/")));
        QVERIFY(!xfdf.contains(QStringLiteral("page=")));
    }

    void serializesModelPagesWithoutOkularAdapter()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        page.annotations.append(baseAnnotation(Mu::Model::AnnotationType::Text, 0.1, 0.2, 0.5, 0.6));

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(xfdf.contains(QStringLiteral("<text ")));
        QVERIFY(xfdf.contains(QStringLiteral("page=\"0\"")));
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,40,100,80\"")));
        QVERIFY(xfdf.contains(QStringLiteral("title=\"alice\"")));
    }

    void scalesDevicePixelsBackToPoints()
    {
        // Okular stores page dimensions in device pixels (points * dpi/72). At
        // 96 dpi a 200x100 pt page is 266.67x133.33 px; serializing must undo the
        // scaling so annotation coordinates stay on the PDF point grid.
        auto* page = makePage(200.0 * 96.0 / 72.0, 100.0 * 96.0 / 72.0);
        auto* annotation = new Okular::TextAnnotation();
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.6));
        page->addAnnotation(annotation);

        const QString xfdf = annotationsToXfdf({ page }, QSizeF(96, 96));
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,40,100,80\"")));
    }

    void convertsRectToUserSpace()
    {
        // Normalized (0.1, 0.2)-(0.5, 0.6) on a 200x100 page: Y flips because
        // Okular counts downward while PDF user-space counts upward.
        const Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::Text, 0.1, 0.2, 0.5, 0.6);
        const Mu::Model::Quad quad = normalizedRectToUserSpace(annotation, 200, 100);
        QCOMPARE(quad.lowerLeft.x, 20.0);
        QCOMPARE(quad.lowerLeft.y, 40.0);
        QCOMPARE(quad.upperRight.x, 100.0);
        QCOMPARE(quad.upperRight.y, 80.0);
    }

    void emitsTextAnnotation()
    {
        auto* page = makePage();
        auto* annotation = new Okular::TextAnnotation();
        annotation->setContents(QStringLiteral("hello <world> & \"friends\""));
        annotation->setAuthor(QStringLiteral("bob"));
        annotation->setUniqueName(QStringLiteral("u1"));
        annotation->setCreationDate(QDateTime::fromMSecsSinceEpoch(-1000, QTimeZone::UTC));
        annotation->setModificationDate(QDateTime::fromMSecsSinceEpoch(0, QTimeZone::UTC));
        annotation->setFlags(Okular::Annotation::Hidden | Okular::Annotation::FixedSize
                             | Okular::Annotation::DenyPrint);
        annotation->style().setColor(QColor(0, 255, 0));
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.6));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<text ")));
        QVERIFY(xfdf.contains(QStringLiteral("page=\"0\"")));
        QVERIFY(xfdf.contains(QStringLiteral("color=\"#00ff00\"")));
        QVERIFY(xfdf.contains(QStringLiteral("title=\"bob\"")));
        QVERIFY(xfdf.contains(QStringLiteral("flags=\"hidden,nozoom\"")));
        QVERIFY(xfdf.contains(QStringLiteral("creationdate=\"D:19691231235959+00'00'\"")));
        QVERIFY(xfdf.contains(QStringLiteral("date=\"D:19700101000000+00'00'\"")));
        // Special characters are XML-escaped in contents.
        QVERIFY(xfdf.contains(QStringLiteral("hello &lt;world&gt; &amp; &quot;friends&quot;")));
    }

    void stripsXmlIllegalControlCharacters()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::Text, 0.1, 0.2, 0.5, 0.6);
        // Vertical tab and a NUL are illegal in XML 1.0; tab and newline are kept.
        annotation.contents = std::string("a\vb\0c\td\ne", 9);
        annotation.author = "x\fy";
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(!xfdf.contains(QLatin1Char('\v')));
        QVERIFY(!xfdf.contains(QLatin1Char('\f')));
        QVERIFY(!xfdf.contains(QLatin1Char('\0')));
        QVERIFY(xfdf.contains(QStringLiteral("<contents>abc\td\ne</contents>")));
        QVERIFY(xfdf.contains(QStringLiteral("title=\"xy\"")));

        QXmlStreamReader reader(xfdf);
        while (!reader.atEnd())
            reader.readNext();
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    }

    void replacesNonFiniteCoordinatesWithZero()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::Square,
                                               std::nan(""),
                                               std::numeric_limits<double>::infinity(),
                                               std::numeric_limits<double>::infinity(),
                                               std::nan(""));
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        // No NaN/inf tokens may reach the output; they collapse to 0.
        QVERIFY(!xfdf.contains(QStringLiteral("nan")));
        QVERIFY(!xfdf.contains(QStringLiteral("inf")));
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"0,0,0,0\"")));

        QXmlStreamReader reader(xfdf);
        while (!reader.atEnd())
            reader.readNext();
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    }

    void emitsHighlightWithQuad()
    {
        auto* page = makePage();
        auto* annotation = new Okular::HighlightAnnotation();
        annotation->setHighlightType(Okular::HighlightAnnotation::Highlight);
        annotation->style().setColor(QColor(255, 255, 0));
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.3));
        Okular::HighlightAnnotation::Quad quad;
        // Okular quad order is points 0..3 = lowerLeft..upperLeft.
        quad.setPoint(Okular::NormalizedPoint(0.1, 0.3), 0);
        quad.setPoint(Okular::NormalizedPoint(0.5, 0.3), 1);
        quad.setPoint(Okular::NormalizedPoint(0.5, 0.2), 2);
        quad.setPoint(Okular::NormalizedPoint(0.1, 0.2), 3);
        annotation->highlightQuads().append(quad);
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<highlight")));
        // The worker reports a padded bound, so the rect is derived from the
        // quad union instead: x 0.1..0.5, y 0.2..0.3 on a 200x100 page.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,70,100,80\"")));
        // XFDF retains Acrobat's top-left, top-right, bottom-left, bottom-right
        // order. On a 200x100 page the normalized corners map to user-space Y of
        // (1 - y) * 100, giving 80 for the top edge and 70 for the bottom edge.
        QVERIFY(xfdf.contains(QStringLiteral("coords=\"20,80,100,80,20,70,100,70\"")));
    }

    void emitsMultipleHighlightQuadsInOrder()
    {
        auto* page = makePage();
        auto* annotation = new Okular::HighlightAnnotation();
        annotation->setHighlightType(Okular::HighlightAnnotation::Highlight);
        annotation->style().setColor(QColor(255, 255, 0));
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.3));
        for (double top : { 0.2, 0.4 }) {
            Okular::HighlightAnnotation::Quad quad;
            quad.setPoint(Okular::NormalizedPoint(0.1, top + 0.1), 0);
            quad.setPoint(Okular::NormalizedPoint(0.5, top + 0.1), 1);
            quad.setPoint(Okular::NormalizedPoint(0.5, top), 2);
            quad.setPoint(Okular::NormalizedPoint(0.1, top), 3);
            annotation->highlightQuads().append(quad);
        }
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        // Each quad appends four points; quads keep their document order.
        QVERIFY(xfdf.contains(QStringLiteral("coords=\"20,80,100,80,20,70,100,70,20,60,100,60,20,50,100,50\"")));
        // The rect spans the union of both quads: y 0.2..0.5.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,50,100,80\"")));
    }

    void freeTextCalloutUsesSeparateGeometry()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::FreeText, 0.1, 0.2, 0.3, 0.4);
        // The leader line ends outside the text box and must not expand its rect.
        annotation.extras.callout = { { 0.3, 0.4 }, { 0.8, 0.9 } };
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,60,60,80\"")));
        QVERIFY(xfdf.contains(QStringLiteral("callout=\"60,60,160,10\"")));
        QVERIFY(xfdf.contains(QStringLiteral("intent=\"FreeTextCallout\"")));

        QString error;
        const auto imported = Xfdf::xfdfToAnnotations(xfdf.toUtf8(), { QSizeF(200, 100) }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(imported.applied, 1);
        const Annotation& roundTripped = imported.pages.first().annotations.first();
        QCOMPARE(roundTripped.x0, 0.1);
        QCOMPARE(roundTripped.y0, 0.2);
        QCOMPARE(roundTripped.x1, 0.3);
        QCOMPARE(roundTripped.y1, 0.4);
        QCOMPARE(roundTripped.extras.callout.size(), 2U);
        QCOMPARE(roundTripped.extras.callout[0].x, 0.3);
        QCOMPARE(roundTripped.extras.callout[0].y, 0.4);
        QCOMPARE(roundTripped.extras.callout[1].x, 0.8);
        QCOMPARE(roundTripped.extras.callout[1].y, 0.9);
        QCOMPARE(roundTripped.extras.style.intent, Mu::Model::AnnotationIntent::FreeTextCallout);
    }

    void importsThreePointFreeTextCallout()
    {
        const QByteArray xml =
            R"(<xfdf><annots><freetext page="0" rect="20,60,60,80" callout="60,60,100,30,160,10"><contents>note</contents></freetext></annots></xfdf>)";
        QString error;
        const auto imported = Xfdf::xfdfToAnnotations(xml, { QSizeF(200, 100) }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(imported.applied, 1);
        const Annotation& annotation = imported.pages.first().annotations.first();
        QCOMPARE(annotation.extras.callout.size(), 3U);
        QCOMPARE(annotation.extras.style.intent, Mu::Model::AnnotationIntent::FreeTextCallout);
    }

    void rejectsMalformedFreeTextCallout()
    {
        const QStringList malformed {
            QStringLiteral("1,2,3"),
            QStringLiteral("1,2,3,4,5,6,7,8"),
            QStringLiteral("1,").repeated(100000),
        };
        for (const QString& callout : malformed) {
            const QByteArray xml =
                QStringLiteral(
                    "<xfdf><annots><freetext page=\"0\" rect=\"20,60,60,80\" callout=\"%1\"/></annots></xfdf>")
                    .arg(callout)
                    .toUtf8();
            QString error;
            const auto imported = Xfdf::xfdfToAnnotations(xml, { QSizeF(200, 100) }, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(imported.applied, 0);
            QCOMPARE(imported.skipped, 1);
        }
    }

    void rejectsCalloutWithIncompatibleIntent()
    {
        const QByteArray xml =
            R"(<xfdf><annots><freetext page="0" rect="20,60,60,80" callout="60,60,160,10" intent="FreeTextTypewriter"/></annots></xfdf>)";
        QString error;
        const auto imported = Xfdf::xfdfToAnnotations(xml, { QSizeF(200, 100) }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(imported.applied, 0);
        QCOMPARE(imported.skipped, 1);
        QVERIFY(imported.warnings.first().contains(QStringLiteral("callout intent")));
    }

    void doesNotExportCalloutWithIncompatibleIntent()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::FreeText, 0.1, 0.2, 0.3, 0.4);
        annotation.extras.callout = { { 0.3, 0.4 }, { 0.8, 0.9 } };
        annotation.extras.style.intent = Mu::Model::AnnotationIntent::FreeTextTypewriter;
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(!xfdf.contains(QStringLiteral("callout=")));
        QVERIFY(xfdf.contains(QStringLiteral("intent=\"FreeTextTypewriter\"")));
    }

    void emitsLineEndpointsAndEndingStyles()
    {
        auto* page = makePage();
        auto* annotation = new Okular::LineAnnotation();
        annotation->setLinePoints({ Okular::NormalizedPoint(0.1, 0.1), Okular::NormalizedPoint(0.2, 0.2) });
        annotation->setLineStartStyle(Okular::LineAnnotation::Square);
        annotation->setLineEndStyle(Okular::LineAnnotation::ClosedArrow);
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("start=\"20,90\" end=\"40,80\" tail=\"Square\" head=\"ClosedArrow\"")));
        // The rect spans both endpoints rather than the padded worker bound.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,80,40,90\"")));
        QVERIFY(!xfdf.contains(QStringLiteral(" points=")));
    }

    void emitsPolylineAndPolygonVertices()
    {
        auto* page = makePage();
        auto* polyline = new Okular::LineAnnotation();
        polyline->setLinePoints({ Okular::NormalizedPoint(0.1, 0.1),
                                  Okular::NormalizedPoint(0.2, 0.2),
                                  Okular::NormalizedPoint(0.3, 0.3) });
        polyline->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.3, 0.3));
        page->addAnnotation(polyline);
        auto* polygon = new Okular::LineAnnotation();
        polygon->setLinePoints({ Okular::NormalizedPoint(0.1, 0.1),
                                 Okular::NormalizedPoint(0.2, 0.2),
                                 Okular::NormalizedPoint(0.3, 0.3) });
        polygon->setLineClosed(true);
        polygon->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.3, 0.3));
        page->addAnnotation(polygon);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        const QString vertices = QStringLiteral("<vertices>20,90;40,80;60,70</vertices>");
        QCOMPARE(xfdf.count(QStringLiteral("<polyline ")), 1);
        QCOMPARE(xfdf.count(QStringLiteral("<polygon ")), 1);
        QCOMPARE(xfdf.count(vertices), 2);
        // Both shapes share the same vertex bounds: x 0.1..0.3, y 0.1..0.3.
        QCOMPARE(xfdf.count(QStringLiteral("rect=\"20,70,60,90\"")), 2);
    }

    void emitsFreeTextDefaultAppearanceWithEscapedFontName()
    {
        auto* page = makePage();
        auto* annotation = new Okular::TextAnnotation();
        annotation->setTextType(Okular::TextAnnotation::InPlace);
        QFont font(QStringLiteral("DejaVu Sans"));
        font.setPointSizeF(12);
        annotation->setTextFont(font);
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.6));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<defaultappearance>0 0 0 rg /DejaVu#20Sans 12 Tf</defaultappearance>")));
    }

    void emitsInkGestures()
    {
        auto* page = makePage();
        auto* annotation = new Okular::InkAnnotation();
        QList<Okular::NormalizedPoint> firstPath { Okular::NormalizedPoint(0.1, 0.1),
                                                   Okular::NormalizedPoint(0.2, 0.2) };
        QList<Okular::NormalizedPoint> secondPath { Okular::NormalizedPoint(0.2, 0.3),
                                                    Okular::NormalizedPoint(0.3, 0.4) };
        annotation->setInkPaths({ firstPath, secondPath });
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<ink")));
        QCOMPARE(xfdf.count(QStringLiteral("<inklist>")), 1);
        QCOMPARE(xfdf.count(QStringLiteral("<gesture>")), 2);
        QVERIFY(xfdf.contains(
            QStringLiteral("<inklist><gesture>20,90;40,80</gesture><gesture>40,70;60,60</gesture></inklist>")));
        // The rect spans every ink point: x 0.1..0.3, y 0.1..0.4.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,60,60,90\"")));
    }

    void generatedXfdfIsWellFormedXml()
    {
        auto* page = makePage();
        auto* annotation = new Okular::InkAnnotation();
        annotation->setInkPaths({ { Okular::NormalizedPoint(0.1, 0.1), Okular::NormalizedPoint(0.2, 0.2) },
                                  { Okular::NormalizedPoint(0.2, 0.3), Okular::NormalizedPoint(0.3, 0.4) } });
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.3, 0.4));
        page->addAnnotation(annotation);

        QXmlStreamReader reader(generatorXfdf({ page }));
        while (!reader.atEnd())
            reader.readNext();
        delete page;

        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    }

    void skipsUnsupportedSubtypes()
    {
        auto* page = makePage();
        // A file attachment has no XFDF mapping and must not be emitted.
        auto* attachment = new Okular::FileAttachmentAnnotation();
        attachment->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
        page->addAnnotation(attachment);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(!xfdf.contains(QStringLiteral("fileattachment")));
        QVERIFY(!xfdf.contains(QStringLiteral("page=\"0\"")));
    }
};

QTEST_MAIN(TestGeneratorXfdf)
#include "test_xfdf.moc"
