// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"
#include "genpdf.hpp"

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

    void testDehyphenatedCharacterFiltering_data()
    {
        QTest::addColumn<int>("lineFlags");
        QTest::addColumn<int>("codepoint");
        QTest::addColumn<bool>("hasNext");
        QTest::addColumn<bool>("hasNextLine");
        QTest::addColumn<bool>("expectedSkip");

        QTest::newRow("ordinary-hyphen") << 0 << static_cast<int>('-') << false << false << false;
        QTest::newRow("joined-line-hyphen")
            << static_cast<int>(FZ_STEXT_LINE_FLAGS_JOINED) << static_cast<int>('-') << false << true << true;
        QTest::newRow("joined-line-nonterminal-hyphen")
            << static_cast<int>(FZ_STEXT_LINE_FLAGS_JOINED) << static_cast<int>('-') << true << true << false;
        QTest::newRow("terminal-line-hyphen")
            << static_cast<int>(FZ_STEXT_LINE_FLAGS_JOINED) << static_cast<int>('-') << false << false << false;
        QTest::newRow("soft-hyphen") << 0 << 0x00AD << false << false << true;
    }

    void testDehyphenatedCharacterFiltering()
    {
        QFETCH(int, lineFlags);
        QFETCH(int, codepoint);
        QFETCH(bool, hasNext);
        QFETCH(bool, hasNextLine);
        QFETCH(bool, expectedSkip);

        fz_stext_line line { };
        line.flags = static_cast<decltype(line.flags)>(lineFlags);
        fz_stext_char next { };
        fz_stext_line nextLine { };
        nextLine.first_char = &next;
        line.next = hasNextLine ? &nextLine : nullptr;
        fz_stext_char character { };
        character.c = codepoint;
        character.next = hasNext ? &next : nullptr;

        QCOMPARE(Mu::Worker::Engine::shouldSkipDehyphenatedChar(&line, &character), expectedSkip);
        const bool expectedJoin = hasNextLine && (lineFlags & FZ_STEXT_LINE_FLAGS_JOINED) != 0;
        QCOMPARE(Mu::Worker::Engine::isDehyphenatedLine(&line), expectedJoin);
        nextLine.first_char = nullptr;
        QVERIFY(!Mu::Worker::Engine::isDehyphenatedLine(&line));
    }

    void testDehyphenatedDocumentText_data()
    {
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<QString>("expectedText");
        QTest::newRow("terminal-hyphen") << QByteArray("BT /F1 12 Tf 72 700 Td (command -) Tj ET")
                                         << QStringLiteral("command -\n");
        QTest::newRow("wrapped-word") << QByteArray("BT /F1 12 Tf 72 700 Td (inter-) Tj 0 -18 Td (national) Tj ET")
                                      << QStringLiteral("international\n");
        QTest::newRow("wrapped-word-terminal-hyphen")
            << QByteArray("BT /F1 12 Tf 72 700 Td (inter-) Tj 0 -18 Td (national-) Tj ET")
            << QStringLiteral("international-\n");
    }

    void testDehyphenatedDocumentText()
    {
        QFETCH(QByteArray, contents);
        QFETCH(QString, expectedText);
        const QString path = m_tempDir.filePath(QStringLiteral("dehyphenation.pdf"));
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, path, contents.constData());
        fz_drop_context(context);

        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, path));
        std::string error;
        const auto boxes = document.textBoxes(0, 72, 72, 1000, true, &error);
        QVERIFY2(error.empty(), error.c_str());
        QString text;
        for (const auto& box : boxes) {
            text += QString::fromStdString(box.text);
            if (box.endOfLine)
                text += QLatin1Char('\n');
        }
        QCOMPARE(text, expectedText);
    }

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
        // The fixture is a blank page: every RGBA byte must be opaque white.
        QVERIFY(std::all_of(buffer.begin(), buffer.end(), [](auto byte) { return byte == 255; }));

        std::vector<std::uint8_t> tileBuffer(200 * 300 * 4);
        QVERIFY2(
            document.renderToBuffer({ 0, 400, 600, Mu::Worker::Engine::DocumentBase::RenderTile { 0, 0, 200, 300 } },
                                    tileBuffer.data(),
                                    200 * 4,
                                    &error),
            error.c_str());
        QVERIFY(std::all_of(tileBuffer.begin(), tileBuffer.end(), [](auto byte) { return byte == 255; }));
    }

    void layersPreserveSiblingDepths_data()
    {
        QTest::addColumn<int>("extraDepth");
        QTest::newRow("direct children") << 0;
        QTest::newRow("one unnamed group") << 1;
        QTest::newRow("two unnamed groups") << 2;
    }

    void layersPreserveSiblingDepths()
    {
        QFETCH(int, extraDepth);
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, QStringLiteral(TEST_SIGNATURE_PDF_DIR "/layers.pdf")));
        fz_context* context = document.context();
        pdf_document* pdf = pdf_specifics(context, document.document());
        pdf_obj* defaults = pdf_dict_getp(context, pdf_trailer(context, pdf), "Root/OCProperties/D");
        pdf_obj* order = pdf_dict_get(context, defaults, PDF_NAME(Order));
        for (int i = 0; i < extraDepth; ++i) {
            pdf_obj* wrapper = pdf_new_array(context, pdf, 1);
            pdf_array_push(context, wrapper, pdf_array_get(context, order, 3));
            pdf_array_put(context, order, 3, wrapper);
            pdf_drop_obj(context, wrapper);
        }
        // Rebuild MuPDF's UI after changing /Order.
        pdf_select_layer_config(context, pdf, -1);
        std::string error;
        const auto entries = document.layers(&error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(entries.size(), std::size_t(9));
        QCOMPARE(entries[2].depth, 0);
        QCOMPARE(entries[3].depth, 1);
        QCOMPARE(entries[4].depth, 1);
        QCOMPARE(entries[5].depth, 0);
        QCOMPARE(entries[6].depth, 1);
        QCOMPARE(entries[7].depth, 1);
    }

    void layersRespectVisibilityAndUiRules()
    {
        using namespace Mu::Model;
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, QStringLiteral(TEST_SIGNATURE_PDF_DIR "/layers.pdf")));
        std::string error;
        auto entries = document.layers(&error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(entries.size(), std::size_t(9));
        const auto findId = [&](const std::string& name) {
            const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.name == name; });
            return it == entries.end() ? -1 : it->id;
        };
        const int red = findId("Red (initially on)");
        const int blue = findId("Blue (initially off)");
        const int parent = findId("Nested parent (initially on)");
        const int childB = findId("Child B (initially off)");
        const int variantA = findId("Variant A");
        const int variantB = findId("Variant B");
        const int locked = findId("Locked reference (always on)");
        const int label = findId("Exclusive choice");
        QVERIFY(red >= 0 && blue >= 0 && parent >= 0 && childB >= 0);
        QVERIFY(variantA >= 0 && variantB >= 0 && locked >= 0 && label >= 0);
        QCOMPARE(entries[static_cast<std::size_t>(childB)].depth, 1);
        QCOMPARE(entries[static_cast<std::size_t>(variantB)].type, LayerType::RadioButton);
        QVERIFY(entries[static_cast<std::size_t>(locked)].locked);
        const auto render = [&] {
            QImage image(612, 792, QImage::Format_RGBA8888);
            if (!document.renderToBuffer({ 0, 612, 792, std::nullopt },
                                         image.bits(),
                                         static_cast<std::size_t>(image.bytesPerLine()),
                                         &error))
                return QImage { };
            return image;
        };
        const auto original = render();
        QVERIFY(!original.isNull());
        QVERIFY(document.isPageCached(0));
        const auto text = [&] {
            std::string value;
            for (const auto& box : document.textBoxes(0, 72, 72, 10000))
                value += box.text;
            return value;
        };
        QVERIFY(text().find("BLUE") == std::string::npos);
        QVERIFY2(document.setLayer(blue, true, &error), error.c_str());
        QVERIFY(!document.isPageCached(0));
        const auto withBlue = render();
        QVERIFY(withBlue.pixelColor(340, 210).blue() > withBlue.pixelColor(340, 210).red());
        QVERIFY(text().find("BLUE") != std::string::npos);
        QVERIFY(document.setLayer(childB, true, &error));
        QVERIFY(document.setLayer(parent, false, &error));
        const auto withoutParent = render();
        QCOMPARE(withoutParent.pixelColor(75, 350), original.pixelColor(340, 350));
        QVERIFY(document.setLayer(variantB, true, &error));
        entries = document.layers();
        QVERIFY(!entries[static_cast<std::size_t>(variantA)].selected);
        QVERIFY(entries[static_cast<std::size_t>(variantB)].selected);
        for (int id : { -1, 999, locked, label }) {
            error.clear();
            QVERIFY(!document.setLayer(id, false, &error));
            QVERIFY(!error.empty());
        }
        QVERIFY(document.setLayer(blue, false));
        QVERIFY(document.setLayer(childB, false));
        QVERIFY(document.setLayer(parent, true));
        QVERIFY(document.setLayer(variantA, true));
        QCOMPARE(render(), original);

        // Tiled rendering must use the same visibility state as a full page.
        QVERIFY(document.setLayer(red, false));
        const auto full = render();
        QImage tile(240, 112, QImage::Format_RGBA8888);
        QVERIFY(
            document.renderToBuffer({ 0, 612, 792, Mu::Worker::Engine::DocumentBase::RenderTile { 48, 128, 240, 112 } },
                                    tile.bits(),
                                    static_cast<std::size_t>(tile.bytesPerLine())));
        QCOMPARE(tile, full.copy(48, 128, 240, 112));

        // Viewing state must not change saved defaults.
        const QString saved = m_tempDir.filePath(QStringLiteral("layers-saved.pdf"));
        QFile output(saved);
        QVERIFY(output.open(QIODevice::ReadWrite));
        QVERIFY(document.saveFd(::dup(output.handle())));
        output.close();
        Mu::Worker::Engine::PdfDocument reopened;
        QVERIFY(openDocument(reopened, saved));
        QVERIFY(reopened.layers()[static_cast<std::size_t>(red)].selected);
        document.close();
        QVERIFY(openDocument(document, m_textPath));
        QVERIFY(document.layers().empty());
    }

    void testOverprintSimulation_data()
    {
        QTest::addColumn<bool>("spot");
        QTest::newRow("process colors") << false;
        QTest::newRow("spot color") << true;
    }

    void testOverprintSimulation()
    {
        QFETCH(bool, spot);
        // Cyan beneath an overprinting yellow rectangle: the overlap should
        // retain cyan when simulated, rather than knock it out.
        const QByteArray content = spot
            ? QByteArray("1 0 0 0 k 10 10 60 60 re f /OP gs /Spot cs 1 scn 40 40 50 50 re f")
            : QByteArray("1 0 0 0 k 10 10 60 60 re f /OP gs 0 0 1 0 k 40 40 50 50 re f");
        const QList<QByteArray> objects {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << "
            "/ExtGState << /OP << /Type /ExtGState /OP true /op true /OPM 1 >> >> "
                + (spot ? QByteArray("/ColorSpace << /Spot [/Separation /Yellow /DeviceCMYK "
                                     "<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0 1 0] /N 1 >>] >> ")
                        : QByteArray())
                + ">> /Contents 4 0 R >>",
            "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream"
        };
        QByteArray pdf("%PDF-1.7\n");
        QList<qsizetype> offsets;
        for (qsizetype i = 0; i < objects.size(); ++i) {
            offsets.append(pdf.size());
            pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const qsizetype xref = pdf.size();
        pdf += "xref\n0 5\n0000000000 65535 f \n";
        for (qsizetype offset : offsets)
            pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        const QString path = m_tempDir.filePath(spot ? "spot-overprint.pdf" : "process-overprint.pdf");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(pdf), pdf.size());
        file.close();

        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, path));
        ::Mu::Model::DocumentSettings settings;
        settings.paperColorRgb = 0xF0E0D0;
        document.setSettings(settings);
        constexpr int width = 100;
        constexpr int stride = width * 4;
        std::vector<std::uint8_t> disabled(width * stride);
        std::string error;
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, disabled.data(), stride, &error),
                 error.c_str());
        settings.overprintSimulation = true;
        document.setSettings(settings);
        std::vector<std::uint8_t> enabled(width * stride);
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, enabled.data(), stride, &error),
                 error.c_str());
        const auto overlap = static_cast<std::size_t>((50 * width + 50) * 4);
        QVERIFY(enabled[overlap] + 30 < disabled[overlap]);
        QCOMPARE(enabled[overlap + 3], std::uint8_t(255));
        // Paper stays opaque; CMYK simulation can round-trip RGB slightly.
        QVERIFY(std::abs(int(enabled[0]) - 0xF0) <= 5);
        QVERIFY(std::abs(int(enabled[1]) - 0xE0) <= 5);
        QVERIFY(std::abs(int(enabled[2]) - 0xD0) <= 5);
        QCOMPARE(enabled[3], std::uint8_t(255));
        constexpr int paddedStride = stride + 12;
        std::vector<std::uint8_t> padded(width * paddedStride, 0xAB);
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, padded.data(), paddedStride, &error),
                 error.c_str());
        for (int y = 0; y < width; ++y) {
            for (int x = 0; x < stride; ++x)
                QCOMPARE(padded[static_cast<std::size_t>(y * paddedStride + x)],
                         enabled[static_cast<std::size_t>(y * stride + x)]);
            for (int x = stride; x < paddedStride; ++x)
                QCOMPARE(padded[static_cast<std::size_t>(y * paddedStride + x)], std::uint8_t(0xAB));
        }
        constexpr int tileSize = 40;
        std::vector<std::uint8_t> tile(tileSize * tileSize * 4);
        QVERIFY2(document.renderToBuffer(
                     { 0, width, width, Mu::Worker::Engine::DocumentBase::RenderTile { 30, 30, tileSize, tileSize } },
                     tile.data(),
                     tileSize * 4,
                     &error),
                 error.c_str());
        for (int y = 0; y < tileSize; ++y)
            for (int x = 0; x < tileSize * 4; ++x)
                QCOMPARE(tile[static_cast<std::size_t>(y * tileSize * 4 + x)],
                         enabled[static_cast<std::size_t>((y + 30) * stride + 30 * 4 + x)]);
        settings.overprintSimulation = false;
        document.setSettings(settings);
        std::vector<std::uint8_t> restored(width * stride);
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, restored.data(), stride, &error),
                 error.c_str());
        QVERIFY(restored == disabled);
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
    }

    void testPageCacheReuseAndEviction()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));
        QVERIFY(!document.isPageCached(0));

        std::vector<std::uint8_t> buffer(40 * 60 * 4);
        std::string error;
        const auto render = [&](int page) {
            return document.renderToBuffer({ page, 40, 60, std::nullopt }, buffer.data(), 40 * 4, &error);
        };

        QVERIFY2(render(0), error.c_str());
        QVERIFY(document.isPageCached(0));

        // A second render of the same page reuses the cached handle.
        QVERIFY2(render(0), error.c_str());
        QVERIFY(document.isPageCached(0));

        // The cache holds three pages: earlier entries survive new loads.
        QVERIFY2(render(1), error.c_str());
        QVERIFY(document.isPageCached(0));
        QVERIFY(document.isPageCached(1));
        QVERIFY2(render(2), error.c_str());
        QVERIFY(document.isPageCached(0));
        QVERIFY(document.isPageCached(1));
        QVERIFY(document.isPageCached(2));

        // Touching page 0 makes it most recently used, so loading page 3
        // evicts page 1, the least recently used entry.
        QVERIFY2(render(0), error.c_str());
        QVERIFY2(render(3), error.c_str());
        QVERIFY(document.isPageCached(0));
        QVERIFY(document.isPageCached(2));
        QVERIFY(document.isPageCached(3));
        QVERIFY(!document.isPageCached(1));

        document.close();
        QVERIFY(!document.isPageCached(0));
        QVERIFY(!document.isPageCached(2));
        QVERIFY(!document.isPageCached(3));
    }

    void testAnnotationWriteKeepsCachedRenderCurrent()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));

        const auto render = [&document](std::vector<std::uint8_t>& pixels) {
            std::string error;
            pixels.resize(120 * 160 * 4);
            return document.renderToBuffer({ 0, 120, 160, std::nullopt }, pixels.data(), 120 * 4, &error);
        };

        std::vector<std::uint8_t> before;
        QVERIFY(render(before));
        QVERIFY(document.isPageCached(0));

        ::Mu::Model::Annotation annotation;
        annotation.subtype = ::Mu::Model::AnnotationType::Highlight;
        annotation.uuid = "cache-invalidation";
        annotation.x0 = .1;
        annotation.y0 = .2;
        annotation.x1 = .5;
        annotation.y1 = .3;
        annotation.color = 0xffff0000U;
        annotation.extras.quads.push_back({ { .1, .2 }, { .5, .2 }, { .5, .3 }, { .1, .3 } });
        std::int32_t object = -1;
        std::string error;
        QVERIFY2(document.addAnnotation(0, annotation, &object, &error), error.c_str());
        QVERIFY(object > 0);
        // The annotation is created on the cached page handle, so it stays valid.
        QVERIFY(document.isPageCached(0));

        std::vector<std::uint8_t> after;
        QVERIFY(render(after));
        QVERIFY(before != after);

        QVERIFY2(document.removeAnnotation(0, object, &error), error.c_str());
        QVERIFY(document.isPageCached(0));
        std::vector<std::uint8_t> restored;
        QVERIFY(render(restored));
        QCOMPARE(restored, before);
    }

    void testSaveClearsPageCache()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        std::vector<std::uint8_t> buffer(40 * 60 * 4);
        std::string error;
        for (int page = 0; page < 3; ++page) {
            QVERIFY2(document.renderToBuffer({ page, 40, 60, std::nullopt }, buffer.data(), 40 * 4, &error),
                     error.c_str());
            QVERIFY(document.isPageCached(page));
        }

        const QString saved = m_tempDir.filePath("cache-save.pdf");
        QFile savedFile(saved);
        QVERIFY(savedFile.open(QIODevice::WriteOnly));
        QVERIFY2(document.saveFd(savedFile.handle(), &error), error.c_str());
        savedFile.close();
        for (int page = 0; page < 3; ++page)
            QVERIFY(!document.isPageCached(page));
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
        pdf_dict_puts_drop(context, malformedPage, "MediaBox", pdf_new_string(context, "malformed", 9));
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
