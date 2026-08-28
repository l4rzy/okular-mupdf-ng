/***************************************************************************
 *   Copyright (C) 2026 by l4rzy <me@23ro.org>                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "engine/pdf/document.hpp"
#include "genpdf.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <unistd.h>

extern "C" {
#include <mupdf/pdf.h>
}

namespace {

bool openDocument(Mu::Worker::Engine::PdfDocument& document, const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    std::string error;
    if (!document.openFd(::dup(file.handle()), path.toStdString(), &error))
        return false;
    return true;
}

} // namespace

class TestPage : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_multiPagePath;
    QString m_singlePagePath;
    QString m_textPath;

private slots:

    void initTestCase()
    {
        QVERIFY(m_tempDir.isValid());
        m_multiPagePath = m_tempDir.filePath("multi.pdf");
        m_singlePagePath = m_tempDir.filePath("single.pdf");
        m_textPath = m_tempDir.filePath("text.pdf");

        fz_context* context = fz_new_context(nullptr, nullptr, 64 * 1024 * 1024);
        QVERIFY(context);
        fz_register_document_handlers(context);
        createMultiPagePDF(context, m_multiPagePath, 35);
        createMultiPagePDF(context, m_singlePagePath, 1);
        createTextPDF(context, m_textPath);
        fz_drop_context(context);
    }

    void testPageGeometry()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));
        const auto geometry = document.pageGeometry(0);
        QCOMPARE(geometry.widthPoints, 1.0);
        QCOMPARE(geometry.heightPoints, 1.0);
        QCOMPARE(QString::fromStdString(geometry.label), QStringLiteral("1"));
    }

    void testTextPDFGeometry()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        const auto geometry = document.pageGeometry(0);
        QCOMPARE(geometry.widthPoints, 612.0);
        QCOMPARE(geometry.heightPoints, 792.0);
        QCOMPARE(QString::fromStdString(geometry.label), QStringLiteral("1"));
    }

    void testRender()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));
        std::string error;
        std::vector<std::uint8_t> buffer(400 * 600 * 4);
        QVERIFY2(document.renderToBuffer({ 0, 400, 600, std::nullopt }, buffer.data(), 400 * 4, &error), error.c_str());

        std::vector<std::uint8_t> tileBuffer(200 * 300 * 4);
        QVERIFY2(
            document.renderToBuffer({ 0, 400, 600, Mu::Worker::Engine::DocumentBase::RenderTile { 0, 0, 200, 300 } },
                                    tileBuffer.data(),
                                    200 * 4,
                                    &error),
            error.c_str());
    }

    void testRenderRejectsInvalidDimensionsAndIsDeterministic()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        std::vector<std::uint8_t> buffer(400 * 400 * 4);
        for (const auto& request : { Mu::Worker::Engine::DocumentBase::RenderRequest { 0, 0, 100, std::nullopt },
                                     Mu::Worker::Engine::DocumentBase::RenderRequest { 0, -1, 100, std::nullopt },
                                     Mu::Worker::Engine::DocumentBase::RenderRequest { 0, 100, 0, std::nullopt } }) {
            std::string error;
            QVERIFY(!document.renderToBuffer(request, buffer.data(), 400 * 4, &error));
            QCOMPARE(error, std::string("render dimensions are invalid"));
        }
        std::vector<std::uint8_t> first(120 * 160 * 4);
        std::vector<std::uint8_t> second(120 * 160 * 4);
        QVERIFY(document.renderToBuffer({ 0, 120, 160, std::nullopt }, first.data(), 120 * 4));
        QVERIFY(document.renderToBuffer({ 0, 120, 160, std::nullopt }, second.data(), 120 * 4));
        QCOMPARE(first, second);
        const QByteArray firstBytes(reinterpret_cast<const char*>(first.data()), static_cast<qsizetype>(first.size()));
        const QByteArray secondBytes(reinterpret_cast<const char*>(second.data()),
                                     static_cast<qsizetype>(second.size()));
        QCOMPARE(QCryptographicHash::hash(firstBytes, QCryptographicHash::Sha256),
                 QCryptographicHash::hash(secondBytes, QCryptographicHash::Sha256));
    }

    void testTextBoxesAndLinks()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        std::string error;
        const auto boxes = document.textBoxes(0, 72, 72, 10000, /*skipAnnots=*/false, &error);
        QVERIFY2(!boxes.empty(), error.c_str());
        QString extracted;
        for (const auto& box : boxes)
            extracted.append(QString::fromUtf8(box.text));
        QVERIFY(extracted.contains(QStringLiteral("Hello World")));

        // skipAnnots=true must return no more boxes than the full run
        const auto boxesContentOnly = document.textBoxes(0, 72, 72, 10000, /*skipAnnots=*/true, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(boxesContentOnly.size() <= boxes.size());

        QVERIFY(document.extractLinks(0, &error).empty());
        QVERIFY(document.extractAnnotations(0, &error).empty());
    }

    void testInternalLinkDestinationCoordinates()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        const auto xyz = document.resolveLink("#page=2&zoom=nan,0.25,0.75");
        QVERIFY(xyz.valid);
        QCOMPARE(xyz.viewport.coordinateMask, uint8_t(3));
        QCOMPARE(xyz.viewport.page, 1);
        QCOMPARE(xyz.viewport.normalizedX, 0.25);
        QCOMPARE(xyz.viewport.normalizedY, 0.75);

        const auto fitH = document.resolveLink("#page=3&view=FitH,0.5");
        QVERIFY(fitH.valid);
        QCOMPARE(fitH.viewport.page, 2);
        QCOMPARE(fitH.viewport.coordinateMask, uint8_t(Mu::Model::Viewport::CoordinateY));
        QCOMPARE(fitH.viewport.normalizedX, 0.0);
        QCOMPARE(fitH.viewport.normalizedY, 0.5);

        const auto fitV = document.resolveLink("#page=4&view=FitV,0.5");
        QVERIFY(fitV.valid);
        QCOMPARE(fitV.viewport.page, 3);
        QCOMPARE(fitV.viewport.coordinateMask, uint8_t(Mu::Model::Viewport::CoordinateX));
        QCOMPARE(fitV.viewport.normalizedX, 0.5);
        QCOMPARE(fitV.viewport.normalizedY, 0.0);

        const auto fitR = document.resolveLink("#page=5&viewrect=0.25,0.1,0.5,0.5");
        QVERIFY(fitR.valid);
        QCOMPARE(fitR.viewport.page, 4);
        QCOMPARE(fitR.viewport.coordinateMask, uint8_t(3));
        QCOMPARE(fitR.viewport.normalizedX, 0.25);
        QVERIFY(std::abs(fitR.viewport.normalizedY - 0.1) < 0.0001);

        const auto fitBh = document.resolveLink("#page=6&view=FitBH,0.25");
        QVERIFY(fitBh.valid);
        QCOMPARE(fitBh.viewport.page, 5);
        QCOMPARE(fitBh.viewport.coordinateMask, uint8_t(Mu::Model::Viewport::CoordinateY));
        QCOMPARE(fitBh.viewport.normalizedX, 0.0);
        QCOMPARE(fitBh.viewport.normalizedY, 0.25);

        const auto fitBv = document.resolveLink("#page=7&view=FitBV,0.25");
        QVERIFY(fitBv.valid);
        QCOMPARE(fitBv.viewport.page, 6);
        QCOMPARE(fitBv.viewport.coordinateMask, uint8_t(Mu::Model::Viewport::CoordinateX));
        QCOMPARE(fitBv.viewport.normalizedX, 0.25);
        QCOMPARE(fitBv.viewport.normalizedY, 0.0);
    }

    void testExplicitDestinationUsesTopLeftPageCoordinates()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));

        const auto destination = document.resolveLink("#page=1&zoom=nan,69.04297,78.73584");
        QVERIFY(destination.valid);
        QCOMPARE(destination.viewport.page, 0);
        QCOMPARE(destination.viewport.coordinateMask, uint8_t(3));
        QVERIFY(std::abs(destination.viewport.normalizedX - (69.04297 / 612.0)) < 0.0001);
        QVERIFY(std::abs(destination.viewport.normalizedY - ((78.73584 - 16.0) / 792.0)) < 0.0001);
    }

    void testTransformedPageAndMalformedPageObject()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        fz_context* context = document.context();
        pdf_document* pdfDocument = pdf_specifics(context, document.document());
        QVERIFY(pdfDocument);
        pdf_page* page = pdf_load_page(context, pdfDocument, 1);
        QVERIFY(page);
        fz_try(context)
        {
            pdf_drop_page(context, page);
            page = nullptr;
            pdf_obj* pageObject = pdf_lookup_page_obj(context, pdfDocument, 1);
            pdf_dict_put_rect(context, pageObject, PDF_NAME(MediaBox), { 10, 20, 510, 620 });
            pdf_dict_put_rect(context, pageObject, PDF_NAME(CropBox), { 110, 120, 410, 520 });
            pdf_dict_put_int(context, pageObject, PDF_NAME(Rotate), 90);
        }
        fz_always(context)
        {
            if (page)
                pdf_drop_page(context, page);
        }
        fz_catch(context)
        {
            QFAIL(fz_caught_message(context));
        }

        const auto transformed = document.resolveLink("#page=2&zoom=nan,260,320");
        QVERIFY(transformed.valid);
        QCOMPARE(transformed.viewport.page, 1);
        QCOMPARE(transformed.viewport.coordinateMask, uint8_t(3));
        QVERIFY(std::abs(transformed.viewport.normalizedX - 0.65) < 0.0001);
        QVERIFY(std::abs(transformed.viewport.normalizedY - (1.0 - 16.0 / 300.0)) < 0.0001);

        pdf_obj* malformedPage = pdf_lookup_page_obj(context, pdfDocument, 2);
        QVERIFY(malformedPage);
        pdf_dict_puts(context, malformedPage, "MediaBox", pdf_new_string(context, "malformed", 9));
        const auto malformed = document.resolveLink("#page=3&zoom=nan,0.5,0.5");
        QVERIFY(malformed.valid);
        QCOMPARE(malformed.viewport.page, 2);
        QCOMPARE(malformed.viewport.coordinateMask, uint8_t(3));
        QVERIFY(std::isfinite(malformed.viewport.normalizedX));
        QVERIFY(std::isfinite(malformed.viewport.normalizedY));

        std::string error;
        const auto malformedDetails = document.pageDetails(2, &error);
        QVERIFY(error.empty());
        QVERIFY(std::isfinite(malformedDetails.geometry.widthPoints));
        QVERIFY(std::isfinite(malformedDetails.geometry.heightPoints));
        error.clear();
        const auto unavailable = document.pageDetails(document.pageCount(), &error);
        QVERIFY(unavailable.annotations.empty());
        QVERIFY(!error.empty());
    }

    void testLinkValidityAndDocumentLifetime()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        const std::string uri = "#page=2&view=Fit";
        const auto first = document.resolveLink(uri);
        QVERIFY(first.valid);
        QCOMPARE(first.viewport.page, 1);
        QCOMPARE(first.viewport.coordinateMask, uint8_t(0));

        const auto external = document.resolveLink("https://example.com/document.pdf");
        QVERIFY(external.valid);
        QVERIFY(external.external);
        QCOMPARE(external.uri, std::string("https://example.com/document.pdf"));

        QVERIFY(!document.resolveLink("#page=999&view=Fit").valid);
        QVERIFY(!document.resolveLink("#nameddest=missing").valid);

        document.close();
        QVERIFY(openDocument(document, m_singlePagePath));
        const auto reopened = document.resolveLink(uri);
        QVERIFY(!reopened.valid);
    }

    void testGeometryUsesPdfPoints()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        const auto geometry = document.pageGeometry(0);
        QVERIFY(geometry.widthPoints > 0.0);
        QVERIFY(geometry.heightPoints > 0.0);
        QCOMPARE(geometry.duration, -1.0);
    }

    void testHighlightAnnotationQuadRoundTrip()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        fz_context* context = document.context();
        pdf_page* page = pdf_load_page(context, pdf_specifics(context, document.document()), 0);
        QVERIFY(page);
        fz_try(context)
        {
            pdf_annot* annotation = pdf_create_annot(context, page, PDF_ANNOT_HIGHLIGHT);
            const fz_quad quad { { 60, 80 }, { 180, 80 }, { 60, 100 }, { 180, 100 } };
            pdf_set_annot_quad_points(context, annotation, 1, &quad);
            pdf_set_annot_name(context, annotation, "highlight-quad-round-trip");
            pdf_update_annot(context, annotation);
            pdf_drop_annot(context, annotation);
        }
        fz_catch(context)
        {
            QFAIL(fz_caught_message(context));
        }
        pdf_drop_page(context, page);

        const QString savedPath = m_tempDir.filePath("highlight.pdf");
        QFile output(savedPath);
        QVERIFY(output.open(QIODevice::WriteOnly));
        std::string error;
        QVERIFY2(document.saveFd(output.handle(), &error), error.c_str());
        output.close();
        document.close();

        Mu::Worker::Engine::PdfDocument reloaded;
        QVERIFY(openDocument(reloaded, savedPath));
        const auto annotations = reloaded.extractAnnotations(0, &error);
        const auto found =
            std::find_if(annotations.cbegin(), annotations.cend(), [](const ::Mu::Model::Annotation& annotation) {
                return annotation.uuid == "highlight-quad-round-trip";
            });
        QVERIFY(found != annotations.cend());
        QCOMPARE(found->extras.quads.size(), size_t(1));
    }
};

int runTestWorkerPage(int argc, char** argv)
{
    TestPage test;
    return QTest::qExec(&test, argc, argv);
}

#include "page.moc"
