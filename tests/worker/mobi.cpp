// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/mobi/document.hpp"
#include "engine/pdf/document.hpp"
#include "plugin/util/document_type.hpp"
#include "plugin/worker_client.hpp"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using namespace Mu;

class TestMobi : public QObject {
    Q_OBJECT

private slots:

    void detectsMobi_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("suffix");
        QTest::addColumn<bool>("mobi");
        QTest::newRow("content") << QStringLiteral("application/x-mobipocket-ebook") << QString() << true;
        QTest::newRow("misleading-suffix")
            << QStringLiteral("application/x-mobipocket-ebook") << QStringLiteral("pdf") << true;
        QTest::newRow("suffix-fallback") << QStringLiteral("application/octet-stream") << QStringLiteral("mobi")
                                         << true;
        QTest::newRow("wrong-content") << QStringLiteral("image/png") << QStringLiteral("mobi") << false;
        QTest::newRow("zip") << QStringLiteral("application/zip") << QStringLiteral("mobi") << false;
    }

    void detectsMobi()
    {
        QFETCH(QString, mime);
        QFETCH(QString, suffix);
        QFETCH(bool, mobi);
        QCOMPARE(Plugin::Util::resolveDocumentType(mime, suffix) == Model::DocumentType::Mobi, mobi);
        QCOMPARE(Model::documentTypeFromMime(Model::documentTypeToMime(Model::DocumentType::Mobi)),
                 Model::DocumentType::Mobi);
    }

    void opensLegacyMobi_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::newRow("uncompressed") << QStringLiteral("legacy-uncompressed.mobi");
        QTest::newRow("palmdoc") << QStringLiteral("legacy-palmdoc.mobi");
    }

    void opensLegacyMobi()
    {
        QFETCH(QString, fixture);
        const QString path = QStringLiteral(TEST_MOBI_DIR "/") + fixture;
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(Plugin::Util::documentTypeForFile(path), Model::DocumentType::Mobi);
        QCOMPARE(Plugin::Util::documentTypeForData(file.readAll()), Model::DocumentType::Mobi);
        QVERIFY(file.seek(0));

        Worker::Engine::MobiDocument document;
        Model::DocumentSettings settings;
        settings.epub.pageSize = Model::EpubPageSize::Letter;
        settings.epub.fontSize = 14;
        document.setSettings(settings);
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "misnamed.pdf", &error), error.c_str());
        QVERIFY(document.pageCount() > 0);
        QVERIFY(!document.isLocked());
        QCOMPARE(document.metadata({ }, &error).mimeType, Model::documentTypeToMime(Model::DocumentType::Mobi));
        const auto geometry = document.pageGeometry(0, &error);
        QVERIFY(geometry.widthPoints > 600);
        QVERIFY(!document.textBoxes(0, 72, 72, 10000, true, &error).empty());
        const auto outline = document.outline(&error);
        QCOMPARE(outline.size(), std::size_t { 1 });
        QCOMPARE(outline.front().title, std::string("Legacy MOBI"));
        QVERIFY(outline.front().link.valid);
        QCOMPARE(outline.front().link.viewport.page, 0);
        const auto links = document.extractLinks(0, &error);
        QVERIFY(std::any_of(links.begin(), links.end(), [](const auto& link) {
            return link.target.external && link.target.uri == "https://example.org/";
        }));
        std::vector<std::uint8_t> pixels(320U * 240U * 4U);
        const Worker::Engine::DocumentBase::RenderRequest request { 0, 320, 240, std::nullopt };
        QVERIFY2(document.renderToBuffer(request, pixels.data(), 320U * 4U, &error), error.c_str());
        QVERIFY2(error.empty(), error.c_str());
    }

    void readsExplicitToc_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<int>("fontSize");
        QTest::newRow("uncompressed") << QStringLiteral("toc-uncompressed.mobi") << 11;
        QTest::newRow("palmdoc") << QStringLiteral("toc-palmdoc.mobi") << 11;
        QTest::newRow("guide-at-first-link") << QStringLiteral("toc-link-start.mobi") << 11;
        QTest::newRow("cp1252-byte-offsets") << QStringLiteral("toc-cp1252.mobi") << 11;
        QTest::newRow("larger-font") << QStringLiteral("toc-palmdoc.mobi") << 18;
    }

    void readsExplicitToc()
    {
        if (!TEST_BUNDLED_MOBI_TOC)
            QSKIP("Explicit MOBI TOC requires the bundled MuPDF patch");
        QFETCH(QString, fixture);
        QFETCH(int, fontSize);
        QFile file(QStringLiteral(TEST_MOBI_DIR "/") + fixture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        Worker::Engine::MobiDocument document;
        Model::DocumentSettings settings;
        settings.epub.fontSize = fontSize;
        document.setSettings(settings);
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "toc.mobi", &error), error.c_str());
        const auto outline = document.outline(&error);
        QCOMPARE(outline.size(), std::size_t { 2 });
        QCOMPARE(outline[0].title, std::string("First café"));
        QCOMPARE(outline[0].children.size(), std::size_t { 1 });
        QCOMPARE(outline[0].children[0].title, std::string("Detail"));
        QCOMPARE(outline[1].title, std::string("Second"));
        const std::array<const Model::OutlineNode*, 3> entries { &outline[0], &outline[0].children[0], &outline[1] };
        const std::array<std::string, 3> expectedText { "First", "Detail", "Second" };
        int previousPage = -1;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& link = entries[i]->link;
            QVERIFY(link.valid);
            QVERIFY(!link.external);
            QVERIFY(link.viewport.page > previousPage);
            previousPage = link.viewport.page;
            std::string text;
            for (const auto& box : document.textBoxes(link.viewport.page, 72, 72, 10000, true, &error))
                text += box.text;
            QVERIFY2(text.find(expectedText[i]) != std::string::npos, text.c_str());
        }
        bool foundInternalLink = false;
        for (int page = 0; page < document.pageCount(); ++page) {
            const auto links = document.extractLinks(page, &error);
            foundInternalLink |= std::any_of(links.begin(), links.end(), [&](const auto& link) {
                return link.target.valid && !link.target.external
                    && link.target.viewport.page == outline[0].link.viewport.page;
            });
        }
        QVERIFY(foundInternalLink);
        QVERIFY2(error.empty(), error.c_str());
    }

    void ignoresInvalidTocGuide_data()
    {
        QTest::addColumn<QByteArray>("position");
        QTest::newRow("overflow") << QByteArray("9999999999");
        QTest::newRow("outside-text") << QByteArray("0000999999");
        QTest::newRow("negative") << QByteArray("-000000001");
        QTest::newRow("nondigit") << QByteArray("000000000x");
    }

    void ignoresInvalidTocGuide()
    {
        if (!TEST_BUNDLED_MOBI_TOC)
            QSKIP("Explicit MOBI TOC requires the bundled MuPDF patch");
        QFETCH(QByteArray, position);
        QFile source(QStringLiteral(TEST_MOBI_DIR "/toc-uncompressed.mobi"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QByteArray bytes = source.readAll();
        const auto marker = bytes.indexOf("type=\"toc\" filepos=\"");
        QVERIFY(marker >= 0);
        bytes.replace(marker + QByteArray("type=\"toc\" filepos=\"").size(), 10, position);
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("invalid-guide.mobi")));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QCOMPARE(file.write(bytes), bytes.size());
        QVERIFY(file.flush());
        Worker::Engine::MobiDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "invalid-guide.mobi", &error), error.c_str());
        QVERIFY(document.outline(&error).empty());
        QVERIFY(document.pageCount() > 0);
        QVERIFY2(error.empty(), error.c_str());
    }

    void exportsExplicitToc()
    {
        if (!TEST_BUNDLED_MOBI_TOC)
            QSKIP("Explicit MOBI TOC requires the bundled MuPDF patch");
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(client.open(QStringLiteral(TEST_MOBI_DIR "/toc-palmdoc.mobi"), { }, pages, Model::DocumentType::Mobi),
                 Model::OpenStatus::Success);
        const auto outline = client.synopsis();
        QCOMPARE(outline.size(), std::size_t { 2 });
        QCOMPARE(outline[0].children.size(), std::size_t { 1 });
        QTemporaryDir directory;
        const auto target = directory.filePath(QStringLiteral("toc.pdf"));
        QSignalSpy finished(&client, &Plugin::WorkerClient::pdfExportFinished);
        QVERIFY(client.startPdfExport(target, { }));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        QFile file(target);
        QVERIFY(file.open(QIODevice::ReadOnly));
        Worker::Engine::PdfDocument pdf;
        std::string error;
        QVERIFY2(pdf.openFd(::dup(file.handle()), "toc.pdf", &error), error.c_str());
        const auto bookmarks = pdf.outline(&error);
        QCOMPARE(bookmarks.size(), outline.size());
        QCOMPARE(bookmarks[0].title, outline[0].title);
        QCOMPARE(bookmarks[0].link.viewport.page, outline[0].link.viewport.page);
        QCOMPARE(bookmarks[0].children.size(), outline[0].children.size());
        QCOMPARE(bookmarks[0].children[0].link.viewport.page, outline[0].children[0].link.viewport.page);
        QCOMPARE(bookmarks[1].link.viewport.page, outline[1].link.viewport.page);
        QVERIFY2(error.empty(), error.c_str());
    }

    void rejectsUnsupportedMobi_data()
    {
        QTest::addColumn<int>("offset");
        QTest::addColumn<int>("value");
        QTest::newRow("magic") << 60 << 0;
        QTest::newRow("compression") << 97 << 99;
        QTest::newRow("encrypted") << 109 << 1;
        QTest::newRow("kf8") << 135 << 8;
        QTest::newRow("invalid-record-offset") << 78 << 255;
        QTest::newRow("truncated") << -1 << 0;
    }

    void rejectsUnsupportedMobi()
    {
        QFETCH(int, offset);
        QFETCH(int, value);
        QFile source(QStringLiteral(TEST_MOBI_DIR "/legacy-uncompressed.mobi"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QByteArray bytes = source.readAll();
        if (offset < 0)
            bytes.truncate(90);
        else
            bytes[offset] = static_cast<char>(value);
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("invalid.mobi")));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QCOMPARE(file.write(bytes), bytes.size());
        QVERIFY(file.flush());
        const int fd = ::dup(file.handle());
        Worker::Engine::MobiDocument document;
        std::string error;
        QVERIFY(!document.openFd(fd, "invalid.mobi", &error));
        QVERIFY(!error.empty());
        QVERIFY(!document.isOpen());
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    }

    void opensAndExportsThroughWorker_data()
    {
        QTest::addColumn<bool>("fromData");
        QTest::newRow("file-and-async-export") << false;
        QTest::newRow("data-and-sync-export") << true;
    }

    void opensAndExportsThroughWorker()
    {
        QFETCH(bool, fromData);
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        const QString path = QStringLiteral(TEST_MOBI_DIR "/legacy-palmdoc.mobi");
        QList<Plugin::WorkerClient::PageInfo> pages;
        if (fromData) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(client.openData(file.readAll(), { }, pages, Model::DocumentType::Mobi),
                     Model::OpenStatus::Success);
        } else {
            QCOMPARE(client.open(path, { }, pages, Model::DocumentType::Mobi), Model::OpenStatus::Success);
        }
        QVERIFY(!pages.empty());
        QCOMPARE(client.getDocumentInfo().mimeType, Model::documentTypeToMime(Model::DocumentType::Mobi));
        const auto image = client.render(0, 320, 240);
        QVERIFY(!image.isNull());
        QCOMPARE(client.render(0, 320, 240, { }, [] { return false; }), image);
        QVERIFY(!client.getTextBoxesForPage(0, 72, 72, true).empty());
        const auto outline = client.synopsis();
        QCOMPARE(outline.size(), std::size_t { 1 });
        QCOMPARE(outline.front().title, std::string("Legacy MOBI"));
        QVERIFY(outline.front().link.valid);
        QCOMPARE(outline.front().link.viewport.page, 0);

        QTemporaryDir directory;
        const QString target = directory.filePath(QStringLiteral("export.pdf"));
        if (fromData) {
            QVERIFY(client.savePdfToFile(target, { }, true));
        } else {
            QSignalSpy finished(&client, &Plugin::WorkerClient::pdfExportFinished);
            const auto job = client.startPdfExport(target, { });
            QVERIFY(job);
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
            QCOMPARE(finished.first().at(0).toULongLong(), *job);
            QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        }
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        Worker::Engine::PdfDocument pdf;
        std::string error;
        QVERIFY2(pdf.openFd(::dup(output.handle()), "export.pdf", &error), error.c_str());
        QCOMPARE(pdf.pageCount(), static_cast<int>(pages.size()));
        QVERIFY(!pdf.textBoxes(0, 72, 72, 10000, true, &error).empty());
        QVERIFY(client.close());
    }
};

int runTestWorkerMobi(int argc, char** argv)
{
    TestMobi test;
    return QTest::qExec(&test, argc, argv);
}

#include "mobi.moc"
