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
#include <limits>
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

    void resolveLinkTable_data()
    {
        QTest::addColumn<QString>("uri");
        QTest::addColumn<bool>("multiPage");
        QTest::addColumn<bool>("expectValid");
        QTest::addColumn<int>("expectPage");
        QTest::addColumn<int>("expectMask");
        QTest::addColumn<double>("expectX");
        QTest::addColumn<double>("expectY");
        QTest::addColumn<bool>("expectExternal");

        constexpr double skip = std::numeric_limits<double>::quiet_NaN();
        constexpr int coordinateX = Mu::Model::Viewport::CoordinateX;
        constexpr int coordinateY = Mu::Model::Viewport::CoordinateY;
        QTest::newRow("xyz") << QStringLiteral("#page=2&zoom=nan,0.25,0.75") << true << true << 1 << 3 << 0.25 << 0.75
                             << false;
        QTest::newRow("fitH") << QStringLiteral("#page=3&view=FitH,0.5") << true << true << 2 << coordinateY << 0.0
                              << 0.5 << false;
        QTest::newRow("fitV") << QStringLiteral("#page=4&view=FitV,0.5") << true << true << 3 << coordinateX << 0.5
                              << 0.0 << false;
        QTest::newRow("fitR") << QStringLiteral("#page=5&viewrect=0.25,0.1,0.5,0.5") << true << true << 4 << 3 << 0.25
                              << 0.1 << false;
        QTest::newRow("fitBH") << QStringLiteral("#page=6&view=FitBH,0.25") << true << true << 5 << coordinateY << 0.0
                               << 0.25 << false;
        QTest::newRow("fitBV") << QStringLiteral("#page=7&view=FitBV,0.25") << true << true << 6 << coordinateX << 0.25
                               << 0.0 << false;
        QTest::newRow("explicitTopLeft") << QStringLiteral("#page=1&zoom=nan,69.04297,78.73584") << false << true << 0
                                         << 3 << 69.04297 / 612.0 << (78.73584 - 16.0) / 792.0 << false;
        QTest::newRow("external") << QStringLiteral("https://example.com/document.pdf") << true << true << -1 << 0
                                  << skip << skip << true;
        QTest::newRow("pageOutOfRange") << QStringLiteral("#page=999&view=Fit") << true << false << -1 << 0 << skip
                                        << skip << false;
        QTest::newRow("missingNamedDest")
            << QStringLiteral("#nameddest=missing") << true << false << -1 << 0 << skip << skip << false;
    }

    void resolveLinkTable()
    {
        QFETCH(QString, uri);
        QFETCH(bool, multiPage);
        QFETCH(bool, expectValid);
        QFETCH(int, expectPage);
        QFETCH(int, expectMask);
        QFETCH(double, expectX);
        QFETCH(double, expectY);
        QFETCH(bool, expectExternal);

        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, multiPage ? m_multiPagePath : m_textPath));

        const auto link = document.resolveLink(uri.toStdString());
        QCOMPARE(link.valid, expectValid);
        QCOMPARE(link.external, expectExternal);
        if (!expectValid)
            return;
        QCOMPARE(link.viewport.page, expectPage);
        QCOMPARE(link.viewport.coordinateMask, static_cast<std::uint8_t>(expectMask));
        if (expectExternal) {
            QCOMPARE(link.uri, std::string("https://example.com/document.pdf"));
            return;
        }
        if (!std::isnan(expectX))
            QVERIFY(std::abs(link.viewport.normalizedX - expectX) < 0.0001);
        if (!std::isnan(expectY))
            QVERIFY(std::abs(link.viewport.normalizedY - expectY) < 0.0001);
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

    void testLinkInvalidatedAfterReopen()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        const std::string uri = "#page=2&view=Fit";
        QVERIFY(document.resolveLink(uri).valid);

        document.close();
        QVERIFY(openDocument(document, m_singlePagePath));
        QVERIFY(!document.resolveLink(uri).valid);
    }
};

int runTestWorkerPage(int argc, char** argv)
{
    TestPage test;
    return QTest::qExec(&test, argc, argv);
}

#include "page.moc"
