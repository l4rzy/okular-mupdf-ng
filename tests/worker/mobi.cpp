// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/document_factory.hpp"
#include "engine/kindle/document.hpp"
#include "engine/mobi/document.hpp"
#include "engine/pdf/document.hpp"
#include "plugin/util/document_type.hpp"
#include "plugin/worker_client.hpp"

#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

using namespace Mu;

#if TEST_BUNDLED_MOBI_TOC
extern "C" fz_archive* fz_extract_html_from_mobi(fz_context*, fz_buffer*);

namespace {

struct AllocationState {
    std::size_t live = 0;
    std::size_t requests = 0;
    std::size_t failAt = 0;

    bool reject()
    {
        ++requests;
        return failAt && requests >= failAt;
    }

    static void* allocate(void* user, std::size_t size)
    {
        auto& state = *static_cast<AllocationState*>(user);
        if (state.reject())
            return nullptr;
        void* result = std::malloc(size);
        if (result)
            ++state.live;
        return result;
    }

    static void release(void* user, void* pointer)
    {
        if (pointer) {
            --static_cast<AllocationState*>(user)->live;
            std::free(pointer);
        }
    }

    static void* resize(void* user, void* pointer, std::size_t size)
    {
        if (!pointer)
            return allocate(user, size);
        if (!size) {
            release(user, pointer);
            return nullptr;
        }
        if (static_cast<AllocationState*>(user)->reject())
            return nullptr;
        return std::realloc(pointer, size);
    }
};

} // namespace
#endif

class TestMobi : public QObject {
    Q_OBJECT

private slots:

    void testDocumentFactory_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<bool>("supported");
        QTest::newRow("pdf") << int(Model::DocumentType::Pdf) << true;
        QTest::newRow("epub") << int(Model::DocumentType::Epub) << true;
        QTest::newRow("mobi") << int(Model::DocumentType::Mobi) << true;
        QTest::newRow("kindle") << int(Model::DocumentType::Kindle) << true;
        QTest::newRow("unknown") << int(Model::DocumentType::Unknown) << false;
    }

    void testDocumentFactory()
    {
        QFETCH(int, type);
        QFETCH(bool, supported);
        const auto expected = static_cast<Model::DocumentType>(type);
        const auto document = Worker::Engine::createDocument(expected, Worker::Engine::Constant::DefaultStoreSize);
        QCOMPARE(bool(document), supported);
        if (document) {
            QCOMPARE(document->type(), expected);
            switch (expected) {
            case Model::DocumentType::Pdf:
                QVERIFY(dynamic_cast<Worker::Engine::PdfDocument*>(document.get()));
                break;
            case Model::DocumentType::Epub:
                QVERIFY(dynamic_cast<Worker::Engine::EpubDocument*>(document.get()));
                break;
            case Model::DocumentType::Mobi:
                QVERIFY(dynamic_cast<Worker::Engine::MobiDocument*>(document.get()));
                break;
            case Model::DocumentType::Kindle:
                QVERIFY(dynamic_cast<Worker::Engine::KindleDocument*>(document.get()));
                break;
            default:
                QFAIL("Unexpected document type");
            }
        }
    }

    void detectsDocumentContent_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<QString>("name");
        QTest::addColumn<Model::DocumentType>("type");
        QTest::newRow("pdf-extensionless")
            << QStringLiteral("../pdf/digital_signature.pdf") << QStringLiteral("book") << Model::DocumentType::Pdf;
        QTest::newRow("pdf-as-epub") << QStringLiteral("../pdf/digital_signature.pdf") << QStringLiteral("book.epub")
                                     << Model::DocumentType::Pdf;
        QTest::newRow("epub-extensionless")
            << QStringLiteral("../epub/sample.epub") << QStringLiteral("book") << Model::DocumentType::Epub;
        QTest::newRow("epub-as-pdf") << QStringLiteral("../epub/sample.epub") << QStringLiteral("book.pdf")
                                     << Model::DocumentType::Epub;
        QTest::newRow("azw3-extensionless")
            << QStringLiteral("sample.azw3") << QStringLiteral("book") << Model::DocumentType::Kindle;
        QTest::newRow("azw3-as-pdf") << QStringLiteral("sample.azw3") << QStringLiteral("book.pdf")
                                     << Model::DocumentType::Kindle;
        QTest::newRow("azw3-as-mobi") << QStringLiteral("sample.azw3") << QStringLiteral("book.mobi")
                                      << Model::DocumentType::Kindle;
        QTest::newRow("kfx-extensionless")
            << QStringLiteral("sample.kfx") << QStringLiteral("book") << Model::DocumentType::Kindle;
        QTest::newRow("kfx-as-pdf") << QStringLiteral("sample.kfx") << QStringLiteral("book.pdf")
                                    << Model::DocumentType::Kindle;
        QTest::newRow("legacy-as-azw3") << QStringLiteral("legacy-palmdoc.mobi") << QStringLiteral("book.azw3")
                                        << Model::DocumentType::Mobi;
        QTest::newRow("legacy-as-kfx") << QStringLiteral("legacy-uncompressed.mobi") << QStringLiteral("book.kfx")
                                       << Model::DocumentType::Mobi;
        QTest::newRow("epub-as-azw3") << QStringLiteral("../epub/sample.epub") << QStringLiteral("book.azw3")
                                      << Model::DocumentType::Epub;
    }

    void detectsDocumentContent()
    {
        QFETCH(QString, fixture);
        QFETCH(QString, name);
        QFETCH(Model::DocumentType, type);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(name);
        QVERIFY(QFile::copy(QStringLiteral(TEST_MOBI_DIR "/") + fixture, path));
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        QCOMPARE(Plugin::Util::documentTypeForFile(path), type);
        QCOMPARE(Plugin::Util::documentTypeForData(input.readAll()), type);
        QCOMPARE(Model::documentTypeFromMime(Model::documentTypeToMime(type)), type);
    }

    void resolvesKindleMime_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("suffix");
        QTest::addColumn<Model::DocumentType>("type");
        for (const auto* suffix : { "azw3", "kfx" }) {
            QTest::newRow(suffix) << QStringLiteral("application/octet-stream") << QString::fromLatin1(suffix)
                                  << Model::DocumentType::Kindle;
        }
        QTest::newRow("mime") << QStringLiteral("application/vnd.amazon.mobi8-ebook") << QString()
                              << Model::DocumentType::Kindle;
        QTest::newRow("alias") << QStringLiteral("application/x-mobi8-ebook") << QStringLiteral("pdf")
                               << Model::DocumentType::Kindle;
        QTest::newRow("pdf") << QStringLiteral("application/pdf") << QStringLiteral("azw3") << Model::DocumentType::Pdf;
        QTest::newRow("image") << QStringLiteral("image/png") << QStringLiteral("kfx") << Model::DocumentType::Unknown;
        QTest::newRow("zip") << QStringLiteral("application/zip") << QStringLiteral("azw3")
                             << Model::DocumentType::Unknown;
    }

    void resolvesKindleMime()
    {
        QFETCH(QString, mime);
        QFETCH(QString, suffix);
        QFETCH(Model::DocumentType, type);
        QCOMPARE(Plugin::Util::resolveDocumentType(mime, suffix), type);
    }

    void resolvesContainerSuffixes_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("suffix");
        QTest::addColumn<Model::DocumentType>("type");

        using Type = Model::DocumentType;

        struct SuffixCase {
            const char* suffix;
            Type binaryType;
            Type zipType;
        };

        constexpr std::array cases {
            SuffixCase { "pdf", Type::Unknown, Type::Unknown }, SuffixCase { "epub", Type::Epub, Type::Epub },
            SuffixCase { "mobi", Type::Mobi, Type::Unknown },   SuffixCase { "azw3", Type::Kindle, Type::Unknown },
            SuffixCase { "kfx", Type::Kindle, Type::Unknown },  SuffixCase { "zip", Type::Unknown, Type::Unknown },
            SuffixCase { "", Type::Unknown, Type::Unknown },
        };
        for (const auto& test : cases) {
            const auto suffix = QString::fromLatin1(test.suffix);
            const auto binaryName = QByteArray("binary-") + test.suffix;
            QTest::newRow(binaryName.constData())
                << QStringLiteral("application/octet-stream") << suffix << test.binaryType;
            for (const auto* mime : { "application/zip",
                                      "application/x-zip",
                                      "application/x-zip-compressed",
                                      "application/java-archive" }) {
                const auto name = QByteArray(mime) + '-' + test.suffix;
                QTest::newRow(name.constData()) << QString::fromLatin1(mime) << suffix << test.zipType;
            }
        }
    }

    void resolvesContainerSuffixes()
    {
        QFETCH(QString, mime);
        QFETCH(QString, suffix);
        QFETCH(Model::DocumentType, type);
        QCOMPARE(Plugin::Util::resolveDocumentType(mime, suffix), type);
    }

    void exportsEpubToAzw3_data()
    {
        QTest::addColumn<bool>("fromData");
        QTest::newRow("file") << false;
        QTest::newRow("data") << true;
    }

    void exportsEpubToAzw3()
    {
        QFETCH(bool, fromData);
        const QString path = QStringLiteral(TEST_EPUB_DIR "/sample.epub");
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        const auto status = fromData ? client.openData(input.readAll(), { }, pages, Model::DocumentType::Epub)
                                     : client.open(path, { }, pages, Model::DocumentType::Epub);
        QCOMPARE(status, Model::OpenStatus::Success);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto rejected = directory.filePath(QStringLiteral("existing.epub"));
        QFile existing(rejected);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("unchanged"), qint64(9));
        existing.close();
        QVERIFY(!client.exportEpubToFile(rejected));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("unchanged"));
        const auto output = directory.filePath(QStringLiteral("book.azw3"));
        QVERIFY(client.exportAzw3ToFile(output));
        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(Plugin::Util::documentTypeForData(result.readAll()), Model::DocumentType::Kindle);
        QVERIFY(result.seek(0));
        Worker::Engine::KindleDocument kindle;
        std::string error;
        QVERIFY2(kindle.openFd(::dup(result.handle()), "book.azw3", &error), error.c_str());
        QVERIFY(kindle.pageCount() > 0);
        QVERIFY(!kindle.textBoxes(0, 72, 72, 10000, true, &error).empty());
    }

    void opensKindle_data()
    {
        QTest::addColumn<bool>("fromData");
        QTest::addColumn<QString>("fixture");
        QTest::newRow("azw3-file") << false << QStringLiteral("sample.azw3");
        QTest::newRow("azw3-data") << true << QStringLiteral("sample.azw3");
        QTest::newRow("kfx-file") << false << QStringLiteral("sample.kfx");
        QTest::newRow("kfx-data") << true << QStringLiteral("sample.kfx");
    }

    void opensKindle()
    {
        QFETCH(bool, fromData);
        QFETCH(QString, fixture);
        const QString path = QStringLiteral(TEST_MOBI_DIR "/") + fixture;
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto bytes = input.readAll();
        QCOMPARE(Plugin::Util::documentTypeForFile(path), Model::DocumentType::Kindle);
        QCOMPARE(Plugin::Util::documentTypeForData(bytes), Model::DocumentType::Kindle);
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        const auto status = fromData ? client.openData(bytes, { }, pages, Model::DocumentType::Kindle)
                                     : client.open(path, { }, pages, Model::DocumentType::Kindle);
        QCOMPARE(status, Model::OpenStatus::Success);
        QVERIFY(!pages.empty());
        QCOMPARE(client.getDocumentInfo().mimeType, Model::documentTypeToMime(Model::DocumentType::Kindle));
        QVERIFY(!client.render(0, 320, 240).isNull());
        const auto boxes = client.getTextBoxesForPage(0, 72, 72, true);
        std::string text;
        for (const auto& box : boxes)
            text += box.text;
        QVERIFY2(text.find("Hello") != std::string::npos, text.c_str());
        QTemporaryDir directory;
        const auto rejected = directory.filePath(QStringLiteral("existing.azw3"));
        QFile existing(rejected);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("unchanged"), qint64(9));
        existing.close();
        QVERIFY(!client.exportAzw3ToFile(rejected));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("unchanged"));
        const auto epubTarget = directory.filePath(QStringLiteral("export.epub"));
        QVERIFY(client.exportEpubToFile(epubTarget));
        QFile epubFile(epubTarget);
        QVERIFY(epubFile.open(QIODevice::ReadOnly));
        QCOMPARE(Plugin::Util::documentTypeForData(epubFile.readAll()), Model::DocumentType::Epub);
        QVERIFY(epubFile.seek(0));
        Worker::Engine::EpubDocument epub;
        std::string epubError;
        QVERIFY2(epub.openFd(::dup(epubFile.handle()), "export.epub", &epubError), epubError.c_str());
        QVERIFY(epub.pageCount() > 0);
        std::string epubText;
        for (const auto& box : epub.textBoxes(0, 72, 72, 10000, true, &epubError))
            epubText += box.text;
        QVERIFY2(epubText.find("Hello") != std::string::npos, epubText.c_str());
        const auto target = directory.filePath(QStringLiteral("export.pdf"));
        if (fromData) {
            QVERIFY(client.savePdfToFile(target, { }, true));
        } else {
            QSignalSpy finished(&client, &Plugin::WorkerClient::pdfExportFinished);
            QVERIFY(client.startPdfExport(target, { }));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
            QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        }
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        Worker::Engine::PdfDocument pdf;
        std::string error;
        QVERIFY2(pdf.openFd(::dup(output.handle()), "export.pdf", &error), error.c_str());
        QCOMPARE(pdf.pageCount(), static_cast<int>(pages.size()));
        QVERIFY(!pdf.textBoxes(0, 72, 72, 10000, true, &error).empty());
    }

    void preservesStyledKindleNavigation_data()
    {
        QTest::addColumn<bool>("fromData");
        QTest::addColumn<int>("fontSize");
        QTest::newRow("file-small-font") << false << 11;
        QTest::newRow("data-large-font") << true << 18;
    }

    void preservesStyledKindleNavigation()
    {
        QFETCH(bool, fromData);
        QFETCH(int, fontSize);
        std::vector<Model::Link> links;
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        connect(&client,
                &Plugin::WorkerClient::pageLinksReady,
                &client,
                [&](quint64, const std::vector<Model::PageLinks>& pages, bool, const QString&) {
                    for (const auto& page : pages)
                        links.insert(links.end(), page.links.begin(), page.links.end());
                });
        Model::DocumentSettings settings;
        settings.epub.fontSize = fontSize;
        QVERIFY(client.setSettings(settings));
        const QString path = QStringLiteral(TEST_MOBI_DIR "/styled-linked.azw3");
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        QList<Plugin::WorkerClient::PageInfo> pages;
        const auto status = fromData ? client.openData(input.readAll(), { }, pages, Model::DocumentType::Kindle)
                                     : client.open(path, { }, pages, Model::DocumentType::Kindle);
        QCOMPARE(status, Model::OpenStatus::Success);
        const auto outline = client.synopsis();
        QVERIFY(outline);
        QCOMPARE(outline->size(), std::size_t { 2 });
        const auto& destination = (*outline)[1];
        QCOMPARE(destination.title, std::string("Middle destination"));
        QVERIFY(destination.link.valid);
        QVERIFY(destination.link.viewport.page > 1);
        std::string text;
        for (const auto& box : client.getTextBoxesForPage(destination.link.viewport.page, 72, 72, true))
            text += box.text;
        QVERIFY2(text.find("Middle") != std::string::npos, text.c_str());
        QTRY_VERIFY(std::any_of(links.begin(), links.end(), [&](const auto& link) {
            return link.target.valid && !link.target.external
                && link.target.viewport.page == destination.link.viewport.page;
        }));

        const auto image = client.render(0, 800, 1000);
        QVERIFY(!image.isNull());
        bool hasStyledText = false;
        for (int y = 0; y < image.height() && !hasStyledText; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixel(x, y);
                hasStyledText |= qRed(pixel) > qGreen(pixel) + 35 && qRed(pixel) > qBlue(pixel) + 20;
            }
        }
        QVERIFY(hasStyledText);

        QTemporaryDir directory;
        const auto target = directory.filePath(QStringLiteral("styled.pdf"));
        QSignalSpy finished(&client, &Plugin::WorkerClient::pdfExportFinished);
        QVERIFY(client.startPdfExport(target, { }));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(2).toString()));
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        Worker::Engine::PdfDocument pdf;
        std::string error;
        QVERIFY2(pdf.openFd(::dup(output.handle()), "styled.pdf", &error), error.c_str());
        const auto bookmarks = pdf.outline(&error);
        QCOMPARE(bookmarks.size(), outline->size());
        QCOMPARE(bookmarks[1].link.viewport.page, destination.link.viewport.page);
        text.clear();
        for (const auto& box : pdf.textBoxes(bookmarks[1].link.viewport.page, 72, 72, 10000, true, &error))
            text += box.text;
        QVERIFY2(text.find("Middle") != std::string::npos, text.c_str());
    }

    void rejectsTruncatedKfx()
    {
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        const QByteArray bytes("CONT\0\0", 6);
        QCOMPARE(Plugin::Util::documentTypeForData(bytes), Model::DocumentType::Kindle);
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(client.openData(bytes, { }, pages, Model::DocumentType::Kindle), Model::OpenStatus::Failed);
        QVERIFY(client.isConnected());
    }

    void rejectsInvalidAzw3_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<bool>("oversized");
        QTest::newRow("truncated") << QStringLiteral("invalid-truncated.mobi") << false;
        QTest::newRow("encrypted") << QStringLiteral("invalid-encrypted.mobi") << false;
        QTest::newRow("oversized") << QStringLiteral("sample.azw3") << true;
        QTest::newRow("invalid-fd") << QString() << false;
    }

    void rejectsInvalidAzw3()
    {
        QFETCH(QString, fixture);
        QFETCH(bool, oversized);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("invalid.azw3")));
        int fd = -1;
        if (!fixture.isEmpty()) {
            QVERIFY(QFile::copy(QStringLiteral(TEST_MOBI_DIR "/") + fixture, file.fileName()));
            QVERIFY(file.open(QIODevice::ReadWrite));
            if (oversized)
                QVERIFY(file.resize(128LL * 1024 * 1024 + 1));
            fd = ::dup(file.handle());
            QVERIFY(fd >= 0);
        }
        Worker::Engine::KindleDocument document;
        QFile valid(QStringLiteral(TEST_MOBI_DIR "/sample.azw3"));
        QVERIFY(valid.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY2(document.openFd(::dup(valid.handle()), "valid.azw3", &error), error.c_str());
        QVERIFY(!document.openFd(fd, "invalid.azw3", &error));
        QVERIFY(!error.empty());
        QVERIFY(!document.isOpen());
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        error.clear();
        QVERIFY2(document.openFd(::dup(valid.handle()), "valid.azw3", &error), error.c_str());
        QVERIFY(document.isOpen());
    }

    void allocationFailures_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::newRow("utf8") << QStringLiteral("toc-uncompressed.mobi");
        QTest::newRow("cp1252") << QStringLiteral("toc-cp1252.mobi");
    }

    void allocationFailures()
    {
#if TEST_BUNDLED_MOBI_TOC
        QFETCH(QString, fixture);
        QFile file(QStringLiteral(TEST_MOBI_DIR "/") + fixture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        std::size_t allocationCount = 0;
        // First count allocations on success, then fail each allocation and
        // all retries. Each iteration owns a fresh context and source buffer.
        for (std::size_t failAt = 0; failAt <= allocationCount; ++failAt) {
            AllocationState state;
            fz_alloc_context allocator {
                &state, AllocationState::allocate, AllocationState::resize, AllocationState::release
            };
            fz_context* ctx = fz_new_context(&allocator, nullptr, 0);
            QVERIFY(ctx);
            fz_set_error_callback(ctx, [](void*, const char*) { }, nullptr);
            fz_buffer* source = nullptr;
            fz_archive* archive = nullptr;
            int error = 0;
            fz_var(source);
            fz_var(archive);
            fz_var(error);
            fz_var(state);
            fz_try(ctx)
            {
                source = fz_new_buffer_from_copied_data(
                    ctx, reinterpret_cast<const unsigned char*>(bytes.constData()), std::size_t(bytes.size()));
                state.requests = 0;
                state.failAt = failAt;
                archive = fz_extract_html_from_mobi(ctx, source);
            }
            fz_catch(ctx)
            {
                error = fz_caught(ctx);
                fz_ignore_error(ctx);
            }
            const auto requests = state.requests;
            state.failAt = 0;
            fz_drop_archive(ctx, archive);
            fz_drop_buffer(ctx, source);
            fz_drop_context(ctx);
            QCOMPARE(state.live, std::size_t { 0 });
            if (failAt == 0) {
                QCOMPARE(error, 0);
                allocationCount = requests;
                QVERIFY(allocationCount > 0);
            } else {
                QCOMPARE(error, int(FZ_ERROR_SYSTEM));
            }
        }
#else
        QSKIP("Allocation cleanup requires the bundled MuPDF patch");
#endif
    }

    void preservesTargetEncoding_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<QByteArray>("before");
        QTest::addColumn<QByteArray>("after");
        QTest::newRow("style") << QString("toc-raw-style.mobi") << QByteArray("</style><span id=")
                               << QByteArray("/* raw destination */</style>");
        QTest::newRow("script") << QString("toc-raw-script.mobi") << QByteArray("</script><span id=")
                                << QByteArray("raw destination\";</script>");
        QTest::newRow("cp1252") << QString("toc-byte-1252.mobi") << QByteArray()
                                << QString::fromUtf8("\"></span>© byte destination").toUtf8();
    }

    void preservesTargetEncoding()
    {
#if TEST_BUNDLED_MOBI_TOC
        QFETCH(QString, fixture);
        QFETCH(QByteArray, before);
        QFETCH(QByteArray, after);
        QFile file(QStringLiteral(TEST_MOBI_DIR "/") + fixture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        Worker::Engine::MobiDocument owner;
        auto* context = owner.context();
        fz_buffer* source = nullptr;
        fz_archive* archive = nullptr;
        fz_buffer* html = nullptr;
        fz_var(source);
        fz_var(archive);
        fz_var(html);
        const auto cleanup = qScopeGuard([&] {
            fz_drop_buffer(context, html);
            fz_drop_archive(context, archive);
            fz_drop_buffer(context, source);
        });
        QByteArray extracted;
        QString error;
        fz_try(context)
        {
            source = fz_new_buffer_from_copied_data(
                context, reinterpret_cast<const unsigned char*>(bytes.constData()), std::size_t(bytes.size()));
            archive = fz_extract_html_from_mobi(context, source);
            html = fz_read_archive_entry(context, archive, "index.html");
            unsigned char* data = nullptr;
            const auto size = fz_buffer_storage(context, html, &data);
            extracted = QByteArray(reinterpret_cast<const char*>(data), qsizetype(size));
        }
        fz_catch(context)
        {
            error = QString::fromUtf8(fz_caught_message(context));
        }
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(extracted.contains(before));
        QVERIFY(extracted.contains(after));
#else
        QSKIP("MOBI targets require the bundled MuPDF patch");
#endif
    }

    void repairsToc_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<QStringList>("roots");
        QTest::addColumn<QStringList>("children");
        QTest::newRow("skipped-levels") << QStringLiteral("toc-skipped-levels.mobi") << QStringList { "First", "Third" }
                                        << QStringList { "Detail", "Second" };
        QTest::newRow("comment-offset") << QStringLiteral("toc-comment.mobi") << QStringList { "First" }
                                        << QStringList { };
        QTest::newRow("comment-bang-close")
            << QStringLiteral("toc-comment-bang.mobi") << QStringList { "First" } << QStringList { };
        QTest::newRow("raw-style") << QStringLiteral("toc-raw-style.mobi") << QStringList { "First" }
                                   << QStringList { };
        QTest::newRow("raw-script") << QStringLiteral("toc-raw-script.mobi") << QStringList { "First" }
                                    << QStringList { };
        QTest::newRow("single-byte-target")
            << QStringLiteral("toc-byte-1252.mobi") << QStringList { "First" } << QStringList { };
    }

    void repairsToc()
    {
        if (!TEST_BUNDLED_MOBI_TOC)
            QSKIP("TOC repair requires the bundled MuPDF patch");
        QFETCH(QString, fixture);
        QFETCH(QStringList, roots);
        QFETCH(QStringList, children);
        QFile file(QStringLiteral(TEST_MOBI_DIR "/") + fixture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        Worker::Engine::MobiDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "toc.mobi", &error), error.c_str());
        const auto outline = document.outline(&error);
        QCOMPARE(outline.size(), std::size_t(roots.size()));
        for (std::size_t i = 0; i < outline.size(); ++i) {
            QCOMPARE(QString::fromStdString(outline[i].title), roots.at(qsizetype(i)));
            QVERIFY(outline[i].link.valid);
            QCOMPARE(outline[i].children.size(), i == 0 ? std::size_t(children.size()) : std::size_t { 0 });
        }
        for (std::size_t i = 0; i < outline.front().children.size(); ++i) {
            const auto& child = outline.front().children[i];
            QCOMPARE(QString::fromStdString(child.title), children.at(qsizetype(i)));
            QVERIFY(child.link.valid);
            QVERIFY(child.children.empty());
        }
        QVERIFY2(error.empty(), error.c_str());
    }

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
        QTest::newRow("utf8") << QStringLiteral("basic-utf8.mobi");
        QTest::newRow("version7") << QStringLiteral("basic-version7.mobi");
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
        QVERIFY(outline);
        QCOMPARE(outline->size(), std::size_t { 2 });
        QCOMPARE((*outline)[0].children.size(), std::size_t { 1 });
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
        QCOMPARE(bookmarks.size(), outline->size());
        QCOMPARE(bookmarks[0].title, (*outline)[0].title);
        QCOMPARE(bookmarks[0].link.viewport.page, (*outline)[0].link.viewport.page);
        QCOMPARE(bookmarks[0].children.size(), (*outline)[0].children.size());
        QCOMPARE(bookmarks[0].children[0].link.viewport.page, (*outline)[0].children[0].link.viewport.page);
        QCOMPARE(bookmarks[1].link.viewport.page, (*outline)[1].link.viewport.page);
        QVERIFY2(error.empty(), error.c_str());
    }

    void rejectsUnsupportedMobi_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<std::string>("expectedError");
        for (const auto* name : { "magic", "truncated" })
            QTest::newRow(name) << QStringLiteral("invalid-%1.mobi").arg(QLatin1String(name))
                                << std::string("invalid legacy MOBI header");
        for (const auto* name : { "record-count", "record-offset", "record-order", "short-record" })
            QTest::newRow(name) << QStringLiteral("invalid-%1.mobi").arg(QLatin1String(name))
                                << std::string("invalid MOBI record table");
        for (const auto* name : { "record-magic", "truncated-record" })
            QTest::newRow(name) << QStringLiteral("invalid-%1.mobi").arg(QLatin1String(name))
                                << std::string("invalid legacy MOBI record header");
        QTest::newRow("compression")
            << QStringLiteral("invalid-compression.mobi")
            << std::string("unsupported MOBI compression; only uncompressed and PalmDOC are supported");
        QTest::newRow("encrypted") << QStringLiteral("invalid-encrypted.mobi")
                                   << std::string("encrypted MOBI documents are not supported");
        QTest::newRow("kf8") << QStringLiteral("invalid-kf8.mobi")
                             << std::string("only legacy MOBI documents are supported");
    }

    void rejectsUnsupportedMobi()
    {
        QFETCH(QString, fixture);
        QFETCH(std::string, expectedError);
        QFile file(QStringLiteral(TEST_MOBI_DIR "/") + fixture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const int fd = ::dup(file.handle());
        QVERIFY(fd >= 0);
        Worker::Engine::MobiDocument document;
        QFile valid(QStringLiteral(TEST_MOBI_DIR "/legacy-uncompressed.mobi"));
        QVERIFY(valid.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY2(document.openFd(::dup(valid.handle()), "valid.mobi", &error), error.c_str());
        QVERIFY(!document.openFd(fd, "invalid.mobi", &error));
        QCOMPARE(error, expectedError);
        QVERIFY(!document.isOpen());
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        error.clear();
        QVERIFY2(document.openFd(::dup(valid.handle()), "valid.mobi", &error), error.c_str());
        QVERIFY(document.isOpen());
    }

    void opensAndExportsThroughWorker_data()
    {
        QTest::addColumn<bool>("fromData");
        QTest::addColumn<QString>("fixture");
        QTest::newRow("file-and-async-export") << false << QStringLiteral("legacy-palmdoc.mobi");
        QTest::newRow("data-and-sync-export") << true << QStringLiteral("legacy-palmdoc.mobi");
        QTest::newRow("utf8-file") << false << QStringLiteral("basic-utf8.mobi");
        QTest::newRow("version7-data") << true << QStringLiteral("basic-version7.mobi");
    }

    void opensAndExportsThroughWorker()
    {
        QFETCH(bool, fromData);
        QFETCH(QString, fixture);
        Plugin::WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        const QString path = QStringLiteral(TEST_MOBI_DIR "/") + fixture;
        QList<Plugin::WorkerClient::PageInfo> pages;
        const QString rejected = QStringLiteral(TEST_MOBI_DIR "/invalid-encrypted.mobi");
        if (fromData) {
            QFile file(rejected);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(client.openData(file.readAll(), { }, pages, Model::DocumentType::Mobi), Model::OpenStatus::Failed);
        } else {
            QCOMPARE(client.open(rejected, { }, pages, Model::DocumentType::Mobi), Model::OpenStatus::Failed);
        }
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
        QVERIFY(outline);
        QCOMPARE(outline->size(), std::size_t { 1 });
        QCOMPARE(outline->front().title, std::string("Legacy MOBI"));
        QVERIFY(outline->front().link.valid);
        QCOMPARE(outline->front().link.viewport.page, 0);

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
