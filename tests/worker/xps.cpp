// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/document_factory.hpp"
#include "engine/xps/document.hpp"
#include "plugin/util/document_type.hpp"
#include "plugin/worker_client.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

using namespace Mu;

namespace {

using ZipEntries = QList<QPair<QByteArray, QByteArray>>;

QByteArray writeZip(const QString& path, const ZipEntries& entries)
{
    fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!context)
        return "could not create context";
    const auto cleanup = qScopeGuard([&] { fz_drop_context(context); });
    const auto encodedPath = QFile::encodeName(path);
    fz_zip_writer* volatile zip = nullptr;
    fz_buffer* volatile buffer = nullptr;
    QByteArray error;
    fz_try(context)
    {
        zip = fz_new_zip_writer(context, encodedPath.constData());
        for (const auto& entry : entries) {
            buffer = fz_new_buffer_from_copied_data(context,
                                                    reinterpret_cast<const unsigned char*>(entry.second.constData()),
                                                    static_cast<std::size_t>(entry.second.size()));
            fz_write_zip_entry(context, zip, entry.first.constData(), buffer, 0);
            fz_drop_buffer(context, buffer);
            buffer = nullptr;
        }
        fz_close_zip_writer(context, zip);
    }
    fz_always(context)
    {
        fz_drop_buffer(context, buffer);
        fz_drop_zip_writer(context, zip);
    }
    fz_catch(context)
    {
        error = fz_caught_message(context);
    }
    return error;
}

ZipEntries singlePageEntries(const QByteArray& width = "400", const QByteArray& height = "600")
{
    return { { "_rels/.rels",
               "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
               "<Relationship Id=\"r1\" Type=\"http://schemas.microsoft.com/xps/2005/06/fixedrepresentation\" "
               "Target=\"/sequence.fdseq\"/></Relationships>" },
             { "sequence.fdseq",
               "<FixedDocumentSequence xmlns=\"http://schemas.microsoft.com/xps/2005/06\">"
               "<DocumentReference Source=\"/document.fdoc\"/></FixedDocumentSequence>" },
             { "document.fdoc",
               "<FixedDocument xmlns=\"http://schemas.microsoft.com/xps/2005/06\">"
               "<PageContent Source=\"/page.fpage\"/></FixedDocument>" },
             { "page.fpage",
               "<FixedPage xmlns=\"http://schemas.microsoft.com/xps/2005/06\" Width=\"" + width + "\" Height=\""
                   + height + "\"/>" } };
}

} // namespace

class TestXps : public QObject {
    Q_OBJECT

private slots:

    void createsXpsDocument()
    {
        const auto document =
            Worker::Engine::createDocument(Model::DocumentType::Xps, Worker::Engine::Constant::DefaultStoreSize);
        QVERIFY(dynamic_cast<Worker::Engine::XpsDocument*>(document.get()));
        QCOMPARE(document->type(), Model::DocumentType::Xps);
    }

    void resolvesXpsMime_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("suffix");
        QTest::addColumn<Model::DocumentType>("type");
        for (const auto* mime : { "application/vnd.ms-xpsdocument", "application/xps", "application/oxps" })
            QTest::newRow(mime) << QString::fromLatin1(mime) << QString() << Model::DocumentType::Xps;
        for (const auto* suffix : { "xps", "oxps" }) {
            QTest::newRow(suffix) << QStringLiteral("application/zip") << QString::fromLatin1(suffix)
                                  << Model::DocumentType::Xps;
        }
        QTest::newRow("pdf-content") << QStringLiteral("application/pdf") << QStringLiteral("xps")
                                     << Model::DocumentType::Pdf;
        QTest::newRow("image-content") << QStringLiteral("image/png") << QStringLiteral("xps")
                                       << Model::DocumentType::Unknown;
        QTest::newRow("plain-zip") << QStringLiteral("application/zip") << QStringLiteral("zip")
                                   << Model::DocumentType::Unknown;
    }

    void resolvesXpsMime()
    {
        QFETCH(QString, mime);
        QFETCH(QString, suffix);
        QFETCH(Model::DocumentType, type);
        QCOMPARE(Plugin::Util::resolveDocumentType(mime, suffix), type);
        QCOMPARE(Model::documentTypeFromMime(Model::documentTypeToMime(type)), type);
    }

    void opensFixture_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("pageCount");
        QTest::newRow("text") << QStringLiteral("text-links.xps") << 2;
        QTest::newRow("openxps") << QStringLiteral("text-links.oxps") << 2;
        QTest::newRow("mixed-pages") << QStringLiteral("mixed-pages.xps") << 3;
        QTest::newRow("graphics") << QStringLiteral("graphics.xps") << 1;
    }

    void opensFixture()
    {
        QFETCH(QString, name);
        QFETCH(int, pageCount);
        const auto path = QStringLiteral(TEST_XPS_DIR "/") + name;
        QCOMPARE(Plugin::Util::documentTypeForFile(path), Model::DocumentType::Xps);
        std::vector<Model::Link> links;
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        connect(&client,
                &Plugin::WorkerClient::pageLinksReady,
                &client,
                [&](quint64, const std::vector<Model::PageLinks>& batch, bool, const QString&) {
                    for (const auto& page : batch)
                        if (page.page == 0)
                            links.insert(links.end(), page.links.begin(), page.links.end());
                });
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(client.open(path, { }, pages, Model::DocumentType::Xps), Model::OpenStatus::Success);
        QCOMPARE(pages.size(), pageCount);
        for (int i = 0; i < pageCount; ++i) {
            QVERIFY(!client.render(i, 120, 160).isNull());
            bool success = false;
            const auto boxes = client.getTextBoxesForPage(i, 72, 72, false, &success);
            QVERIFY(success);
            QVERIFY(!boxes.empty());
        }
        if (name.startsWith(QStringLiteral("text-links"))) {
            std::string text;
            for (const auto& box : client.getTextBoxesForPage(0, 72, 72))
                text += box.text;
            QVERIFY(QString::fromStdString(text).contains(QStringLiteral("Hello XPS. Café résumé. 0123456789")));
            QTRY_COMPARE(links.size(), std::size_t(2));
            bool foundExternal = false;
            bool foundInternal = false;
            for (const auto& link : links) {
                QVERIFY(link.target.valid);
                if (link.target.external) {
                    QCOMPARE(link.target.uri, std::string("https://example.com/"));
                    foundExternal = true;
                } else {
                    QCOMPARE(link.target.viewport.page, 1);
                    foundInternal = true;
                }
            }
            QVERIFY(foundExternal);
            QVERIFY(foundInternal);
        }
        if (name == QStringLiteral("mixed-pages.xps")) {
            QCOMPARE(pages[0].geometry.widthPoints, 300.0);
            QCOMPARE(pages[0].geometry.heightPoints, 450.0);
            QCOMPARE(pages[1].geometry.widthPoints, 600.0);
            QCOMPARE(pages[1].geometry.heightPoints, 300.0);
            QCOMPARE(pages[2].geometry.widthPoints, 360.0);
            QCOMPARE(pages[2].geometry.heightPoints, 360.0);
        }
    }

    void rendersTiles_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("page");
        QTest::addColumn<int>("theme");
        QTest::addColumn<int>("width");
        // Fractional scales exercise rounding when themed images are composited.
        for (const int width : { 240, 234, 245, 250 })
            for (const auto theme : { Model::DocumentTheme::Default, Model::DocumentTheme::Dark }) {
                const auto suffix =
                    QStringLiteral("%1-%2").arg(theme == Model::DocumentTheme::Dark ? "dark" : "default").arg(width);
                QTest::newRow(qPrintable(QStringLiteral("graphics-") + suffix))
                    << QStringLiteral("graphics.xps") << 0 << static_cast<int>(theme) << width;
                QTest::newRow(qPrintable(QStringLiteral("landscape-") + suffix))
                    << QStringLiteral("mixed-pages.xps") << 1 << static_cast<int>(theme) << width;
            }
    }

    void rendersTiles()
    {
        QFETCH(QString, name);
        QFETCH(int, page);
        QFETCH(int, theme);
        QFETCH(int, width);
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        Model::DocumentSettings settings;
        settings.documentTheme = static_cast<Model::DocumentTheme>(theme);
        QVERIFY(client.setSettings(settings));
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(client.open(QStringLiteral(TEST_XPS_DIR "/") + name, { }, pages, Model::DocumentType::Xps),
                 Model::OpenStatus::Success);
        const auto full = client.render(page, width, 320);
        QVERIFY(!full.isNull());
        const QRect tile(35, 45, 140, 190);
        const auto cropped = client.render(page, width, 320, tile);
        QVERIFY(!cropped.isNull());
        const auto expected = full.copy(tile);
        QCOMPARE(cropped.size(), expected.size());
        int maxDelta = 0;
        QPoint maxDeltaPosition;
        for (int y = 0; y < cropped.height(); ++y) {
            for (int x = 0; x < cropped.width(); ++x) {
                const auto actualPixel = cropped.pixelColor(x, y);
                const auto expectedPixel = expected.pixelColor(x, y);
                const int delta = std::max({ std::abs(actualPixel.red() - expectedPixel.red()),
                                             std::abs(actualPixel.green() - expectedPixel.green()),
                                             std::abs(actualPixel.blue() - expectedPixel.blue()),
                                             std::abs(actualPixel.alpha() - expectedPixel.alpha()) });
                if (delta > maxDelta) {
                    maxDelta = delta;
                    maxDeltaPosition = QPoint(x, y) + tile.topLeft();
                }
            }
        }
        // MuPDF gradient shading and image resampling can round differently
        // at tile boundaries. Other fixtures should match exactly.
        const int tolerance = name == QStringLiteral("graphics.xps") ? 2 : 0;
        QVERIFY2(maxDelta <= tolerance,
                 qPrintable(QStringLiteral("Maximum channel difference: %1 at (%2, %3)")
                                .arg(maxDelta)
                                .arg(maxDeltaPosition.x())
                                .arg(maxDeltaPosition.y())));
    }

    void printsMixedPages_data()
    {
        QTest::addColumn<bool>("subset");
        QTest::newRow("all-pages") << false;
        QTest::newRow("reordered-subset") << true;
    }

    void printsMixedPages()
    {
        QFETCH(bool, subset);
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(client.open(QStringLiteral(TEST_XPS_DIR "/mixed-pages.xps"), { }, pages, Model::DocumentType::Xps),
                 Model::OpenStatus::Success);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto output = dir.filePath(QStringLiteral("print.pdf"));
        const QVector<int> selected = subset ? QVector<int> { 2, 0 } : QVector<int> { };
        QVERIFY(client.writePdfToFile(output, selected, Model::PdfOutputMode::Printable));
        QList<Plugin::WorkerClient::PageInfo> printed;
        QCOMPARE(client.open(output, { }, printed, Model::DocumentType::Pdf), Model::OpenStatus::Success);
        QCOMPARE(printed.size(), subset ? 2 : 3);
        for (qsizetype i = 0; i < printed.size(); ++i) {
            const auto source = subset ? selected[i] : i;
            QCOMPARE(printed[i].geometry.widthPoints, pages[source].geometry.widthPoints);
            QCOMPARE(printed[i].geometry.heightPoints, pages[source].geometry.heightPoints);
            QVERIFY(!client.getTextBoxesForPage(static_cast<int>(i), 72, 72).empty());
        }
    }

    void rejectsInvalidInput_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray { };
        QTest::newRow("truncated-zip") << QByteArray("PK\003\004");
        QTest::newRow("plain-text") << QByteArray("not an XPS document");
    }

    void rejectsInvalidInput()
    {
        QFETCH(QByteArray, bytes);
        Worker::Engine::XpsDocument document;
        std::string error;
        const auto source = QFile::encodeName(QStringLiteral(TEST_XPS_DIR "/graphics.xps"));
        QVERIFY(document.openFd(::open(source.constData(), O_RDONLY), "graphics.xps", &error));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile invalid(dir.filePath(QStringLiteral("invalid.xps")));
        QVERIFY(invalid.open(QIODevice::ReadWrite));
        QCOMPARE(invalid.write(bytes), bytes.size());
        QVERIFY(invalid.flush());
        const int fd = ::dup(invalid.handle());
        QVERIFY(fd >= 0);
        QVERIFY(!document.openFd(fd, "invalid.xps", &error));
        QVERIFY(!error.empty());
        QVERIFY(!document.isOpen());
        QCOMPARE(document.pageCount(), 0);
        errno = 0;
        QCOMPARE(::fcntl(fd, F_GETFD), -1);
        QCOMPARE(errno, EBADF);
        error.clear();
        QVERIFY(document.openFd(::open(source.constData(), O_RDONLY), "graphics.xps", &error));
        QVERIFY(error.empty());
        QCOMPARE(document.pageCount(), 1);
    }

    void enumeratesFonts_data()
    {
        QTest::addColumn<QString>("fontName");
        QTest::newRow("truetype") << QStringLiteral("Allura.ttf");
        QTest::newRow("obfuscated") << QStringLiteral("00000000-0000-0000-0000-000000000000.odttf");
    }

    void enumeratesFonts()
    {
        QFETCH(QString, fontName);
        QFile font(QStringLiteral(TEST_XPS_DIR "/../../thirdparty/allura/Allura-Regular.ttf"));
        QVERIFY(font.open(QIODevice::ReadOnly));
        auto entries = singlePageEntries();
        const auto fontPath = QByteArray("Resources/") + fontName.toUtf8();
        // A zero GUID produces an all-zero obfuscation key, so the bytes
        // are unchanged while still exercising XPS's .odttf font loader.
        entries.push_back({ fontPath, font.readAll() });
        entries[3].second =
            "<FixedPage xmlns=\"http://schemas.microsoft.com/xps/2005/06\" Width=\"400\" Height=\"600\">"
            "<Glyphs FontUri=\"/"
            + fontPath
            + "\" FontRenderingEmSize=\"32\" OriginX=\"40\" OriginY=\"70\" Fill=\"#000000\" "
              "UnicodeString=\"Embedded font\"/></FixedPage>";
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("font.xps"));
        const auto failure = writeZip(path, entries);
        QVERIFY2(failure.isEmpty(), failure.constData());
        Worker::Engine::XpsDocument document;
        std::string error;
        QVERIFY(document.openFd(::open(QFile::encodeName(path).constData(), O_RDONLY), "font.xps", &error));
        const auto fonts = document.fonts({ 0 }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(fonts.size(), std::size_t(1));
        QCOMPARE(fonts[0].file, fontPath.toStdString());
        QCOMPARE(fonts[0].type, Model::FontType::TrueType);
        QCOMPARE(fonts[0].embedType, Model::FontEmbedType::FullyEmbedded);
        QCOMPARE(document.fonts({ 0 }, &error).size(), fonts.size());
        QVERIFY(!document.textBoxes(0, 72, 72, 1000, false, &error).empty());
        QVERIFY2(error.empty(), error.c_str());
        document.close();
        QVERIFY(document.fonts({ 0 }, &error).empty());
        QVERIFY(!error.empty());
    }

    void rejectsInvalidRenders_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::newRow("null-pixels") << 0;
        QTest::newRow("short-stride") << 1;
        QTest::newRow("oversized-stride") << 2;
        QTest::newRow("invalid-page") << 3;
        QTest::newRow("invalid-dimensions") << 4;
        QTest::newRow("invalid-tile") << 5;
        QTest::newRow("cancelled") << 6;
    }

    void rejectsInvalidRenders()
    {
        QFETCH(int, scenario);
        Worker::Engine::XpsDocument document;
        std::string error;
        const auto path = QFile::encodeName(QStringLiteral(TEST_XPS_DIR "/graphics.xps"));
        QVERIFY(document.openFd(::open(path.constData(), O_RDONLY), "graphics.xps", &error));
        Worker::Engine::DocumentBase::RenderRequest request { 0, 4, 4, std::nullopt };
        std::array<unsigned char, 64> pixels;
        pixels.fill(0xA5);
        void* destination = pixels.data();
        std::size_t stride = 16;
        fz_cookie cookie { };
        switch (scenario) {
        case 0:
            destination = nullptr;
            break;
        case 1:
            stride = 15;
            break;
        case 2:
            stride = std::numeric_limits<std::size_t>::max();
            break;
        case 3:
            request.page = 1;
            break;
        case 4:
            request.width = 0;
            break;
        case 5:
            request.tile = Worker::Engine::DocumentBase::RenderTile { 3, 3, 2, 2 };
            break;
        case 6:
            cookie.abort = 1;
            request.cookie = &cookie;
            break;
        }
        QVERIFY(!document.renderToBuffer(request, destination, stride, &error));
        QVERIFY(!error.empty());
        for (const auto value : pixels)
            QCOMPARE(value, static_cast<unsigned char>(0xA5));
        error.clear();
        QVERIFY(document.renderToBuffer({ 0, 4, 4, std::nullopt }, pixels.data(), 16, &error));
        QVERIFY(error.empty());
    }

    void hashesDocumentContent_data()
    {
        QTest::addColumn<bool>("fromData");
        QTest::newRow("file") << false;
        QTest::newRow("data") << true;
    }

    void hashesDocumentContent()
    {
        QFETCH(bool, fromData);
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        std::string previous;
        for (const auto* name : { "text-links.xps", "text-links.oxps", "text-links.xps" }) {
            const auto path = QStringLiteral(TEST_XPS_DIR "/") + QString::fromLatin1(name);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            QList<Plugin::WorkerClient::PageInfo> pages;
            QCOMPARE(fromData ? client.openData(bytes, { }, pages, Model::DocumentType::Xps)
                              : client.open(path, { }, pages, Model::DocumentType::Xps),
                     Model::OpenStatus::Success);
            const auto hashOnly = client.getDocumentInfo({ QStringLiteral("hash") });
            QCOMPARE(hashOnly.values.size(), std::size_t(1));
            const auto hash = hashOnly.values.at("hash");
            QCOMPARE(hash, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString());
            if (!previous.empty())
                QVERIFY(hash != previous);
            previous = hash;
            QCOMPARE(client.getDocumentInfo().values.at("hash"), hash);
            const auto formatOnly = client.getDocumentInfo({ QStringLiteral("format") });
            QCOMPARE(formatOnly.values.size(), std::size_t(1));
            QCOMPARE(formatOnly.values.at("format"), std::string("XPS"));
            QVERIFY(!client.render(0, 120, 160).isNull());
        }
    }

    void rejectsOtherFormats_data()
    {
        QTest::addColumn<QString>("source");
        QTest::newRow("pdf") << QStringLiteral(TEST_XPS_DIR "/../pdf/digital_signature.pdf");
        QTest::newRow("epub") << QStringLiteral(TEST_XPS_DIR "/../epub/sample.epub");
    }

    void rejectsOtherFormats()
    {
        QFETCH(QString, source);
        Worker::Engine::XpsDocument document;
        std::string error;
        const auto path = QFile::encodeName(source);
        const int fd = ::open(path.constData(), O_RDONLY);
        QVERIFY(fd >= 0);
        QVERIFY(!document.openFd(fd, "misleading.xps", &error));
        QVERIFY(!error.empty());
        QVERIFY(!document.isOpen());
        errno = 0;
        QCOMPARE(::fcntl(fd, F_GETFD), -1);
        QCOMPARE(errno, EBADF);
    }

    void validatesGeometry_data()
    {
        QTest::addColumn<QByteArray>("width");
        QTest::addColumn<QByteArray>("height");
        QTest::addColumn<bool>("valid");
        QTest::newRow("normal") << QByteArray("400") << QByteArray("600") << true;
        QTest::newRow("zero-width") << QByteArray("0") << QByteArray("600") << false;
        QTest::newRow("negative-height") << QByteArray("400") << QByteArray("-1") << false;
        QTest::newRow("nan-width") << QByteArray("nan") << QByteArray("600") << false;
        QTest::newRow("infinite-height") << QByteArray("400") << QByteArray("inf") << false;
    }

    void validatesGeometry()
    {
        QFETCH(QByteArray, width);
        QFETCH(QByteArray, height);
        QFETCH(bool, valid);
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("geometry.xps"));
        const auto failure = writeZip(path, singlePageEntries(width, height));
        QVERIFY2(failure.isEmpty(), failure.constData());
        Worker::Engine::XpsDocument document;
        std::string error;
        QVERIFY(document.openFd(::open(QFile::encodeName(path).constData(), O_RDONLY), "geometry.xps", &error));
        const auto geometry = document.pageGeometry(0, &error);
        QCOMPARE(error.empty(), valid);
        QCOMPARE(geometry.widthPoints > 0 && geometry.heightPoints > 0, valid);
        error.clear();
        QFile output(dir.filePath(QStringLiteral("print.pdf")));
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(document.savePdfFd(::dup(output.handle()), { }, &error), valid);
        QCOMPARE(error.empty(), valid);
    }

    void validatesOutlineText_data()
    {
        QTest::addColumn<int>("length");
        QTest::addColumn<bool>("valid");
        QTest::newRow("short") << 16 << true;
        QTest::newRow("oversized-title") << static_cast<int>(Limit::MaxString + 1) << false;
    }

    void validatesOutlineText()
    {
        QFETCH(int, length);
        QFETCH(bool, valid);
        auto entries = singlePageEntries();
        entries.push_back(
            { "_rels/document.fdoc.rels",
              "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"r1\" Type=\"http://schemas.microsoft.com/xps/2005/06/documentstructure\" "
              "Target=\"/structure.struct\"/></Relationships>" });
        entries.push_back({ "structure.struct",
                            "<DocumentStructure><DocumentStructure.Outline><DocumentOutline>"
                            "<OutlineEntry OutlineTarget=\"#1\" Description=\""
                                + QByteArray(length, 'x')
                                + "\"/></DocumentOutline></DocumentStructure.Outline></DocumentStructure>" });
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("outline.xps"));
        const auto failure = writeZip(path, entries);
        QVERIFY2(failure.isEmpty(), failure.constData());
        Worker::Engine::XpsDocument document;
        std::string error;
        QVERIFY(document.openFd(::open(QFile::encodeName(path).constData(), O_RDONLY), "outline.xps", &error));
        const auto outline = document.outline(&error);
        QCOMPARE(error.empty(), valid);
        QCOMPARE(outline.size(), valid ? std::size_t(1) : std::size_t(0));
    }

    void validatesLinkBudget()
    {
        Worker::Engine::XpsDocument document;
        std::string error;
        const auto path = QFile::encodeName(QStringLiteral(TEST_XPS_DIR "/text-links.xps"));
        QVERIFY(document.openFd(::open(path.constData(), O_RDONLY), "links.xps", &error));
        Worker::Engine::ExtractionBudgets budgets;
        budgets.links.remainingBytes = 1;
        QVERIFY(document.extractLinks(0, &error, &budgets).empty());
        QVERIFY(!error.empty());
        error.clear();
        QCOMPARE(document.extractLinks(0, &error).size(), std::size_t(2));
        QVERIFY(error.empty());
        QVERIFY(document.textBoxes(0, 72, 72, 1, false, &error).empty());
        QVERIFY(!error.empty());
        error.clear();
        QVERIFY(!document.textBoxes(0, 72, 72, 1000, false, &error).empty());
        QVERIFY(error.empty());
    }

    void probesZipDataInWorker()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("ordinary.zip"));
        const auto failure = writeZip(path, { { "hello.txt", "not XPS" } });
        QVERIFY2(failure.isEmpty(), failure.constData());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QCOMPARE(Plugin::Util::documentTypeForData(bytes), Model::DocumentType::Xps);
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(client.openData(bytes, { }, pages, Plugin::Util::documentTypeForData(bytes)),
                 Model::OpenStatus::Failed);
    }

    void opensXps_data()
    {
        QTest::addColumn<QString>("suffix");
        QTest::addColumn<QString>("schema");
        QTest::addColumn<bool>("fromData");
        for (const bool fromData : { false, true }) {
            QTest::newRow(fromData ? "xps-data" : "xps-file")
                << QStringLiteral("xps") << QStringLiteral("http://schemas.microsoft.com/xps/2005/06") << fromData;
            QTest::newRow(fromData ? "oxps-data" : "oxps-file")
                << QStringLiteral("oxps") << QStringLiteral("http://schemas.openxps.org/oxps/v1.0") << fromData;
        }
    }

    void opensXps()
    {
        QFETCH(QString, suffix);
        QFETCH(QString, schema);
        QFETCH(bool, fromData);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("sample.") + suffix);
        const QByteArray ns = schema.toUtf8();
        const QList<QPair<QByteArray, QByteArray>> entries {
            { "_rels/.rels",
              "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"r1\" Type=\""
                  + ns
                  + "/fixedrepresentation\" Target=\"/sequence.fdseq\"/>"
                    "</Relationships>" },
            { "sequence.fdseq",
              "<FixedDocumentSequence xmlns=\"" + ns
                  + "\"><DocumentReference Source=\"/document.fdoc\"/>"
                    "</FixedDocumentSequence>" },
            { "document.fdoc",
              "<FixedDocument xmlns=\"" + ns
                  + "\"><PageContent Source=\"/one.fpage\"/>"
                    "<PageContent Source=\"/two.fpage\"/></FixedDocument>" },
            { "one.fpage",
              "<FixedPage xmlns=\"" + ns
                  + "\" Width=\"400\" Height=\"600\">"
                    "<Path Fill=\"#ff0000\" Data=\"M 0,0 L 400,0 400,600 0,600 Z\"/></FixedPage>" },
            { "two.fpage", "<FixedPage xmlns=\"" + ns + "\" Width=\"800\" Height=\"400\"/>" }
        };
        const auto failure = writeZip(path, entries);
        QVERIFY2(failure.isEmpty(), failure.constData());
        QCOMPARE(Plugin::Util::documentTypeForFile(path), Model::DocumentType::Xps);
        QVERIFY(!Model::isReflowableDocument(Model::DocumentType::Xps));
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto bytes = input.readAll();
        QCOMPARE(Plugin::Util::documentTypeForData(bytes), Model::DocumentType::Xps);
        const auto status = fromData ? client.openData(bytes, { }, pages, Plugin::Util::documentTypeForData(bytes))
                                     : client.open(path, { }, pages, Model::DocumentType::Xps);
        QCOMPARE(status, Model::OpenStatus::Success);
        QCOMPARE(pages.size(), 2);
        QCOMPARE(pages[0].geometry.widthPoints, 300.0);
        QCOMPARE(pages[0].geometry.heightPoints, 450.0);
        QCOMPARE(pages[1].geometry.widthPoints, 600.0);
        QCOMPARE(pages[1].geometry.heightPoints, 300.0);
        QCOMPARE(client.getDocumentInfo().mimeType, Model::documentTypeToMime(Model::DocumentType::Xps));
        const auto image = client.render(0, 40, 60);
        QVERIFY(!image.isNull());
        QCOMPARE(image.pixelColor(20, 30), QColor(Qt::red));
    }
};

int runTestWorkerXps(int argc, char** argv)
{
    TestXps test;
    return QTest::qExec(&test, argc, argv);
}

#include "xps.moc"
