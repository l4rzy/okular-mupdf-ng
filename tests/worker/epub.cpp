// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/epub/document.hpp"
#include "engine/pdf/document.hpp"
#include "runtime/command_service.hpp"
#include "shared/model/types.hpp"
#include "shared/transport/fd_channel.hpp"
#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <array>
#include <cmath>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

class TestEpub : public QObject {
    Q_OBJECT
private slots:

    void testOpenAndMetadata()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int fd = ::dup(file.handle());
        file.close();
        QVERIFY(fd >= 0);

        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        QVERIFY2(doc.openFd(fd, "sample.epub", &error), error.c_str());
        QVERIFY(doc.isOpen());
        QVERIFY(!doc.isLocked());
        QVERIFY(doc.pageCount() > 0);

        const auto meta = doc.metadata({ "title" }, &error);
        QCOMPARE(QString::fromStdString(meta.mimeType), QStringLiteral("application/epub+zip"));
        QVERIFY(meta.values.contains("title"));
        QCOMPARE(QString::fromStdString(meta.values.at("title")), QStringLiteral("Test EPUB"));
    }

    void testPageDetailsWithoutLinksUsesGeometryOnly()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());

        const auto details = document.pageDetails(0, &error, false);
        QVERIFY2(error.empty(), error.c_str());
        const auto geometry = document.pageGeometry(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.geometry.widthPoints, geometry.widthPoints);
        QCOMPARE(details.geometry.heightPoints, geometry.heightPoints);
        QCOMPARE(details.geometry.label, geometry.label);
        QVERIFY(details.links.empty());
    }

    void testResolveLinkTable_data()
    {
        QTest::addColumn<QString>("uri");
        QTest::addColumn<bool>("expectValid");
        QTest::addColumn<bool>("expectExternal");
        QTest::newRow("https") << QStringLiteral("https://example.com/x") << true << true;
        QTest::newRow("mailto") << QStringLiteral("mailto:a@b.c") << true << true;
        QTest::newRow("empty") << QStringLiteral("") << false << false;
        QTest::newRow("missing-fragment") << QStringLiteral("#missing-anchor") << false << false;
        QTest::newRow("missing-file") << QStringLiteral("nonexistent.html") << false << false;
    }

    void testResolveLinkTable()
    {
        QFETCH(QString, uri);
        QFETCH(bool, expectValid);
        QFETCH(bool, expectExternal);

        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());

        // A null error mirrors the production call sites (outline extraction
        // and page-link iteration), which only need the validity outcome.
        const auto link = document.resolveLink(uri.toStdString(), nullptr);
        QCOMPARE(link.valid, expectValid);
        QCOMPARE(link.external, expectExternal);
        if (expectExternal)
            QCOMPARE(QString::fromStdString(link.uri), uri);
    }

    void testExtractLinksEmptyDocument()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());

        const auto links = document.extractLinks(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(links.empty());
    }

    void testPageDetailsWithLinksMatchesGeometry()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());

        const auto details = document.pageDetails(0, &error, true);
        QVERIFY2(error.empty(), error.c_str());
        const auto geometry = document.pageGeometry(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.geometry.widthPoints, geometry.widthPoints);
        QCOMPARE(details.geometry.heightPoints, geometry.heightPoints);
        // The link-including path reports MuPDF's page label ("ch. 1, p. 1")
        // rather than the index-based label from pageGeometry ("1").
        QVERIFY(!details.geometry.label.empty());
        QVERIFY(details.links.empty());
    }

    void testMetadataUnknownKeyIgnored()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());

        const auto unknown = document.metadata({ "no-such-key" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(unknown.values.empty());

        const auto first = document.metadata({ }, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto second = document.metadata({ }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(first.values.find("hash") != first.values.end());
        QCOMPARE(second.values.at("hash"), first.values.at("hash"));
    }

    void testDocumentAcceleratorRoundTrip()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));

        std::string error;
        ::Mu::Worker::Engine::EpubDocument cold;
        QVERIFY2(cold.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());
        const auto accelerator = cold.exportAccelerator(&error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(!accelerator.empty());

        ::Mu::Worker::Engine::EpubDocument accelerated;
        QVERIFY2(accelerated.openFdWithAccelerator(::dup(file.handle()), "sample.epub", accelerator, &error),
                 error.c_str());
        QCOMPARE(accelerated.pageCount(), cold.pageCount());

        std::vector<std::uint8_t> coldPixels(400U * 300U * 4U);
        std::vector<std::uint8_t> acceleratedPixels(400U * 300U * 4U);
        const ::Mu::Worker::Engine::DocumentBase::RenderRequest request { 0, 400, 300, std::nullopt };
        QVERIFY2(cold.renderToBuffer(request, coldPixels.data(), 400U * 4U, &error), error.c_str());
        QVERIFY2(accelerated.renderToBuffer(request, acceleratedPixels.data(), 400U * 4U, &error), error.c_str());
        QCOMPARE(acceleratedPixels, coldPixels);
    }

    void testRenderHonorsPaperColor()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));

        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());

        // Default settings keep the white background.
        std::vector<std::uint8_t> pixels(400U * 300U * 4U);
        const ::Mu::Worker::Engine::DocumentBase::RenderRequest request { 0, 400, 300, std::nullopt };
        QVERIFY2(document.renderToBuffer(request, pixels.data(), 400U * 4U, &error), error.c_str());
        QCOMPARE(pixels[0], 0xFF);
        QCOMPARE(pixels[3], 0xFF);

        // The Okular paper color replaces the background (last pixel = bottom
        // right corner, away from reflowed content).
        ::Mu::Model::DocumentSettings settings;
        settings.paperColorRgb = 0x112233;
        document.setSettings(settings);
        QVERIFY2(document.renderToBuffer(request, pixels.data(), 400U * 4U, &error), error.c_str());
        constexpr std::size_t lastPixel = (400U * 300U - 1U) * 4U;
        QCOMPARE(pixels[lastPixel], 0x11);
        QCOMPARE(pixels[lastPixel + 1], 0x22);
        QCOMPARE(pixels[lastPixel + 2], 0x33);
        QCOMPARE(pixels[lastPixel + 3], 0xFF);
    }

    void testInvalidAcceleratorFallsBackToColdOpen()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));

        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        const std::vector<std::uint8_t> invalidAccelerator { 0xde, 0xad, 0xbe, 0xef };
        QVERIFY2(document.openFdWithAccelerator(::dup(file.handle()), "sample.epub", invalidAccelerator, &error),
                 error.c_str());
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(document.pageCount() > 0);
        QVERIFY(!document.exportAccelerator(&error).empty());
        QVERIFY2(error.empty(), error.c_str());
    }

    void testFontsFromArchive()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));

        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        QVERIFY2(doc.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());
        file.close();

        const auto first = doc.fonts({ }, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto second = doc.fonts({ 0 }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(first.size(), second.size());
        QVERIFY(first.empty());
    }

    void testSavePdf()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QFile source(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(source.open(QIODevice::ReadOnly));

        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(source.handle()), "sample.epub", &error), error.c_str());
        source.close();

        const QString fullPath = directory.filePath(QStringLiteral("full.pdf"));
        const int fullFd = ::open(fullPath.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fullFd >= 0);
        QVERIFY2(document.savePdfFd(fullFd, { }, &error), error.c_str());

        QFile fullFile(fullPath);
        QVERIFY(fullFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument fullPdf;
        QVERIFY2(fullPdf.openFd(::dup(fullFile.handle()), "full.pdf", &error), error.c_str());
        fullFile.close();
        QCOMPARE(fullPdf.pageCount(), document.pageCount());
        QVERIFY(fullPdf.pageCount() > 0);

        const QString selectedPath = directory.filePath(QStringLiteral("selected.pdf"));
        const int selectedFd = ::open(selectedPath.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        QVERIFY(selectedFd >= 0);
        QVERIFY2(document.savePdfFd(selectedFd, { 0 }, &error), error.c_str());

        QFile selectedFile(selectedPath);
        QVERIFY(selectedFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument selectedPdf;
        QVERIFY2(selectedPdf.openFd(::dup(selectedFile.handle()), "selected.pdf", &error), error.c_str());
        selectedFile.close();
        QCOMPARE(selectedPdf.pageCount(), 1);
    }

    void testRenderAndTextBoxes()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int fd = ::dup(file.handle());
        file.close();

        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        QVERIFY2(doc.openFd(fd, "sample.epub", &error), error.c_str());

        const auto initialGeometry = doc.pageGeometry(0, &error);
        const int initialPageCount = doc.pageCount();

        std::vector<std::uint8_t> buffer1(800 * 600 * 4);
        QVERIFY(doc.renderToBuffer({ 0, 800, 600, std::nullopt }, buffer1.data(), 800 * 4, &error));
        QVERIFY2(error.empty(), error.c_str());

        std::vector<std::uint8_t> buffer2(400 * 800 * 4);
        QVERIFY(doc.renderToBuffer({ 0, 400, 800, std::nullopt }, buffer2.data(), 400 * 4, &error));
        QVERIFY2(error.empty(), error.c_str());

        const std::array tiles {
            ::Mu::Worker::Engine::DocumentBase::RenderTile { 0, 0, 200, 300 },
            ::Mu::Worker::Engine::DocumentBase::RenderTile { 50, 100, 200, 300 },
            ::Mu::Worker::Engine::DocumentBase::RenderTile { 200, 500, 200, 300 },
            ::Mu::Worker::Engine::DocumentBase::RenderTile { 300, 0, 100, 800 },
        };
        for (const auto& tile : tiles) {
            const auto tileWidth = static_cast<std::size_t>(tile.width);
            const auto tileHeight = static_cast<std::size_t>(tile.height);
            const auto tileStride = tileWidth * 4U;
            std::vector<std::uint8_t> tileBuffer(tileWidth * tileHeight * 4U);
            QVERIFY2(doc.renderToBuffer({ 0, 400, 800, tile }, tileBuffer.data(), tileStride, &error), error.c_str());
            QVERIFY2(error.empty(), error.c_str());
            for (int row = 0; row < tile.height; ++row) {
                const auto* expected = buffer2.data()
                    + (static_cast<std::size_t>(tile.y) + static_cast<std::size_t>(row)) * 400U * 4U
                    + static_cast<std::size_t>(tile.x) * 4U;
                const auto* actual = tileBuffer.data() + static_cast<std::size_t>(row) * tileStride;
                QVERIFY(std::equal(actual, actual + tileWidth * 4U, expected));
            }
        }

        const auto paddedTile = tiles[1];
        const std::size_t paddedStride = static_cast<std::size_t>(paddedTile.width) * 4U + 8U;
        std::vector<std::uint8_t> paddedBuffer(paddedStride * paddedTile.height, 0xa5);
        QVERIFY2(doc.renderToBuffer({ 0, 400, 800, paddedTile }, paddedBuffer.data(), paddedStride, &error),
                 error.c_str());
        QVERIFY2(error.empty(), error.c_str());
        for (int row = 0; row < paddedTile.height; ++row) {
            const auto* expected = buffer2.data()
                + (static_cast<std::size_t>(paddedTile.y) + static_cast<std::size_t>(row)) * 400U * 4U
                + static_cast<std::size_t>(paddedTile.x) * 4U;
            const auto* actual = paddedBuffer.data() + static_cast<std::size_t>(row) * paddedStride;
            QVERIFY(std::equal(actual, actual + static_cast<std::size_t>(paddedTile.width) * 4U, expected));
            QVERIFY(std::all_of(actual + static_cast<std::size_t>(paddedTile.width) * 4U,
                                actual + paddedStride,
                                [](std::uint8_t value) { return value == 0xa5; }));
        }

        QCOMPARE(doc.pageCount(), initialPageCount);
        const auto finalGeometry = doc.pageGeometry(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(finalGeometry.widthPoints, initialGeometry.widthPoints);
        QCOMPARE(finalGeometry.heightPoints, initialGeometry.heightPoints);

        const auto boxes = doc.textBoxes(0, 96.0, 96.0, 1000, false, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(!boxes.empty());

        std::string text;
        for (const auto& box : boxes)
            text += box.text;
        QVERIFY2(text.find("Hello") != std::string::npos, text.c_str());

        const auto boxesAt72Dpi = doc.textBoxes(0, 72.0, 72.0, 1000, false, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(boxesAt72Dpi.size(), boxes.size());
        QVERIFY(!boxesAt72Dpi.empty());
        QVERIFY(std::abs(boxes.front().left - boxesAt72Dpi.front().left * (96.0 / 72.0)) < 0.01);
        QVERIFY(std::abs(boxes.front().top - boxesAt72Dpi.front().top * (96.0 / 72.0)) < 0.01);
        QVERIFY(std::abs(boxes.front().right - boxesAt72Dpi.front().right * (96.0 / 72.0)) < 0.01);
        QVERIFY(std::abs(boxes.front().bottom - boxesAt72Dpi.front().bottom * (96.0 / 72.0)) < 0.01);

        QVERIFY(doc.textBoxes(0, 0.0, 72.0, 1000, false, &error).empty());
        QVERIFY(error.find("text DPI is invalid") != std::string::npos);
        error.clear();
        QVERIFY(doc.textBoxes(0, 72.0, 72.0, 1, false, &error).empty());
        QVERIFY(error.find("text box limit exceeded") != std::string::npos);
    }

    void testConfiguredPageSizes()
    {
        constexpr double pointsPerMillimeter = 72.0 / 25.4;

        struct PageSizeCase {
            ::Mu::Model::EpubPageSize size;
            double widthMillimeters;
            double heightMillimeters;
        };

        const std::array cases { PageSizeCase { ::Mu::Model::EpubPageSize::B5, 176, 250 },
                                 PageSizeCase { ::Mu::Model::EpubPageSize::A5, 148, 210 },
                                 PageSizeCase { ::Mu::Model::EpubPageSize::SixByNine, 152, 229 },
                                 PageSizeCase { ::Mu::Model::EpubPageSize::Letter, 216, 279 } };

        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        for (const auto& pageSize : cases) {
            ::Mu::Model::DocumentSettings settings;
            settings.epub.pageSize = pageSize.size;
            settings.epub.fontSize = 17;
            doc.setSettings(settings);
            QVERIFY2(doc.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());

            const auto geometry = doc.pageGeometry(0, &error);
            QVERIFY2(error.empty(), error.c_str());
            QVERIFY(std::abs(geometry.widthPoints - pageSize.widthMillimeters * pointsPerMillimeter) < 0.1);
            QVERIFY(std::abs(geometry.heightPoints - pageSize.heightMillimeters * pointsPerMillimeter) < 0.1);
        }
        file.close();
    }

    void testSettingsChangeOnNextOpen()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;

        ::Mu::Model::DocumentSettings firstSettings;
        firstSettings.epub.pageSize = ::Mu::Model::EpubPageSize::B5;
        doc.setSettings(firstSettings);
        QVERIFY2(doc.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());
        const auto firstGeometry = doc.pageGeometry(0, &error);
        QCOMPARE(QString::fromStdString(firstGeometry.label), QStringLiteral("1"));

        ::Mu::Model::DocumentSettings secondSettings = firstSettings;
        secondSettings.epub.pageSize = ::Mu::Model::EpubPageSize::A5;
        doc.setSettings(secondSettings);
        QCOMPARE(doc.pageGeometry(0, &error).widthPoints, firstGeometry.widthPoints);
        QCOMPARE(doc.pageGeometry(0, &error).heightPoints, firstGeometry.heightPoints);

        QVERIFY2(doc.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());
        const auto secondGeometry = doc.pageGeometry(0, &error);
        QVERIFY(secondGeometry.widthPoints < firstGeometry.widthPoints);
        QVERIFY(secondGeometry.heightPoints < firstGeometry.heightPoints);
        file.close();
    }

    void testCustomCss()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;

        ::Mu::Model::DocumentSettings settings;
        settings.epub.customCssBase64 =
            QByteArrayLiteral("body { display: none !important; }").toBase64().toStdString();
        doc.setSettings(settings);
        QVERIFY2(doc.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());
        QVERIFY(doc.textBoxes(0, 72, 72, 1000, false, &error).empty());
        QVERIFY2(error.empty(), error.c_str());

        settings.epub.customCssBase64 = "not-base64";
        doc.setSettings(settings);
        QVERIFY2(doc.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());
        const auto boxes = doc.textBoxes(0, 72, 72, 1000, false, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(!boxes.empty());
        file.close();
    }

    void testOutline()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int fd = ::dup(file.handle());
        file.close();

        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        QVERIFY2(doc.openFd(fd, "sample.epub", &error), error.c_str());

        const auto outline = doc.outline(&error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(!outline.empty());
        QCOMPARE(QString::fromStdString(outline[0].title), QStringLiteral("Chapter 1"));

        doc.close();
        error.clear();
        QVERIFY(doc.outline(&error).empty());
        QVERIFY(!error.empty());
    }

    void testPdfOnlyOperationsAreGuarded()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int fd = ::dup(file.handle());
        file.close();

        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        QVERIFY(doc.openFd(fd, "sample.epub", &error));

        std::int32_t obj = -1;
        ::Mu::Model::Annotation annot;
        QVERIFY(!doc.addAnnotation(0, annot, &obj, &error));
        QVERIFY(error.find("not supported") != std::string::npos);

        QVERIFY(!doc.saveFd(-1, &error));
        QVERIFY(error.find("not supported") != std::string::npos);

        QVERIFY(!doc.signFd(::Mu::Model::SignRequest { }, nullptr, -1, nullptr, &error));
        QVERIFY(error.find("not supported") != std::string::npos);
    }

    void testCommandServiceDispatch()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int fd = ::dup(file.handle());
        file.close();

        ::Mu::Worker::Runtime::CommandService service({ });
        std::string error;
        QVERIFY2(service.openFd(fd, "sample.epub", ::Mu::Model::DocumentType::Epub, &error), error.c_str());
        QVERIFY(service.document());
        QCOMPARE(service.document()->pageCount(), 1);

        const auto saveResponse = service.saveFdResponse(1, -1);
        QVERIFY(saveResponse.error);
        QCOMPARE(saveResponse.error->code, ::Mu::Model::ErrorCode::Internal);
        QVERIFY(saveResponse.error->message.find("not supported") != std::string::npos);
    }

    void testOcrIsRejectedForEpub()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int documentFd = ::dup(file.handle());
        QVERIFY(documentFd >= 0);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        std::string error;
        ::Mu::IPC::FdChannel receiver;
        const auto socketPath = directory.filePath(QStringLiteral("fd.sock")).toStdString();
        if (!receiver.listen(socketPath, &error))
            QSKIP(qPrintable(QStringLiteral("FD socket unavailable: ") + QString::fromStdString(error)));
        ::Mu::IPC::FdChannel sender;
        QVERIFY(sender.connect(socketPath, &error));
        QVERIFY(receiver.accept(&error));

        ::Mu::Worker::Runtime::CommandService service({ .sandbox = { }, .fdChannel = &receiver });
        QVERIFY2(service.openFd(documentFd, "sample.epub", ::Mu::Model::DocumentType::Epub, &error), error.c_str());

        const int ocrFd = ::dup(file.handle());
        QVERIFY(ocrFd >= 0);
        QVERIFY(sender.send(7, ocrFd, &error));
        ::close(ocrFd);
        const auto response = service.dispatch({ 1, ::Mu::Model::OcrPageRequest { { 7 }, 0, 225, "eng", false } });
        QVERIFY(response.error);
        QCOMPARE(response.error->code, ::Mu::Model::ErrorCode::Unavailable);
        QVERIFY(response.error->message.find("PDF") != std::string::npos);
    }

    void testMetadataFilteringAndHash()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int fd = ::dup(file.handle());
        file.close();

        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        QVERIFY(doc.openFd(fd, "sample.epub", &error));

        // Unfiltered metadata query returns hash
        const auto allMeta = doc.metadata({ }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(allMeta.values.find("hash") != allMeta.values.end());
        QVERIFY(!allMeta.values.at("hash").empty());

        // Filtered metadata query only returns requested keys
        const auto filteredMeta = doc.metadata({ "title" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(filteredMeta.values.find("hash") == filteredMeta.values.end());
        if (filteredMeta.values.find("title") != filteredMeta.values.end()) {
            QCOMPARE(filteredMeta.values.size(), size_t(1));
        }

        // Explicit hash request
        const auto hashOnlyMeta = doc.metadata({ "hash" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(hashOnlyMeta.values.size(), size_t(1));
        QCOMPARE(hashOnlyMeta.values.at("hash"), allMeta.values.at("hash"));
    }
};

int runTestWorkerEpub(int argc, char** argv)
{
    TestEpub test;
    return QTest::qExec(&test, argc, argv);
}

#include "epub.moc"
