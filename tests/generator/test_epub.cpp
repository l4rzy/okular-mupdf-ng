// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/epub_cache.hpp"
#include "support/genpdf.hpp"
#include "support/synopsis.hpp"
#include "support/text_inspection.hpp"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDateTime>
#include <QFile>
#include <QMimeDatabase>
#include <QPrinter>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QUrl>

#include <okular/core/action.h>
#include <okular/core/document.h>
#include <okular/core/generator.h>
#include <okular/core/observer.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

#include <mupdf/fitz.h>

#include <algorithm>
#include <cmath>

namespace {

struct PdfInspection {
    int images = 0;
    bool embeddedFont = false;
    bool overlappingText = false;
    QString error;
    Mu::Test::TextInspection text;
};

PdfInspection inspectPdf(const QString& path)
{
    PdfInspection result;
    fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!context) {
        result.error = QStringLiteral("Could not create MuPDF context");
        return result;
    }
    const auto cleanup = qScopeGuard([&] { fz_drop_context(context); });
    // Keep Qt temporaries outside MuPDF's setjmp/longjmp region.
    const QByteArray encodedPath = QFile::encodeName(path);
    fz_document* document = nullptr;
    fz_stext_page* text = nullptr;
    fz_var(result);
    fz_var(document);
    fz_var(text);
    fz_try(context)
    {
        fz_register_document_handlers(context);
        document = fz_open_document(context, encodedPath.constData());
        fz_stext_options options { };
        options.flags = FZ_STEXT_PRESERVE_IMAGES | FZ_STEXT_ACCURATE_BBOXES;
        for (int page = 0; page < fz_count_pages(context, document); ++page) {
            text = fz_new_stext_page_from_page_number(context, document, page, &options);
            for (auto* block = text->first_block; block; block = block->next) {
                if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                    ++result.images;
                    for (auto* other = text->first_block; other; other = other->next) {
                        if (other->type != FZ_STEXT_BLOCK_TEXT)
                            continue;
                        for (auto* line = other->u.t.first_line; line; line = line->next) {
                            // Ignore sub-point edge rounding; a caption must remain below its image.
                            const auto intersection = fz_intersect_rect(block->bbox, line->bbox);
                            if (intersection.x1 - intersection.x0 > 1 && intersection.y1 - intersection.y0 > 1)
                                result.overlappingText = true;
                        }
                    }
                } else if (block->type == FZ_STEXT_BLOCK_TEXT) {
                    for (auto* line = block->u.t.first_line; line; line = line->next)
                        for (auto* ch = line->first_char; ch; ch = ch->next)
                            if (QByteArray(fz_font_name(context, ch->font)).contains("Allura"))
                                result.embeddedFont = true;
                }
            }
            fz_drop_stext_page(context, text);
            text = nullptr;
        }
        result.text = Mu::Test::inspectDocumentText(context, document);
    }
    fz_always(context)
    {
        fz_drop_stext_page(context, text);
        fz_drop_document(context, document);
    }
    fz_catch(context)
    {
        result.error = QString::fromUtf8(fz_caught_message(context));
    }
    return result;
}

} // namespace

class TestGeneratorEpub : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;

    void reloadPluginSettings()
    {
        // EPUB layout settings take effect on the next generator instance.
        // Reload through Okular, then destroy this temporary PDF session.
        Okular::Document document(nullptr);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        QCOMPARE(document.openDocument(path,
                                       QUrl::fromLocalFile(path),
                                       QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
    }

    void checkPage(Okular::Document& document, Okular::DocumentObserver& observer, int page)
    {
        document.requestTextPage(static_cast<unsigned int>(page));
        QTRY_VERIFY_WITH_TIMEOUT(document.page(page)->hasTextPage(), 10000);
        document.requestPixmaps(
            { new Okular::PixmapRequest(&observer, page, 600, 850, 1, 0, Okular::PixmapRequest::Asynchronous) });
        QTRY_VERIFY_WITH_TIMEOUT(document.page(page)->hasPixmap(&observer), 10000);
        const auto text = document.page(page)->text().remove(' ').remove('\n');
        QVERIFY(text.contains(page == 0 ? QStringLiteral("Captionbeneathdiagram.") : QStringLiteral("Aftertable.")));
        QVERIFY(text.contains(page == 0 ? QStringLiteral("Aftercaption.") : QStringLiteral("Seven")));
        const auto linksReady = [&] {
            const auto& objects = document.page(page)->objectRects();
            return std::any_of(objects.cbegin(), objects.cend(), [](const auto* object) {
                return object->objectType() == Okular::ObjectRect::Action;
            });
        };
        QVERIFY(linksReady()); // Links must already exist when the first pixmap is published.
        const auto& objects = document.page(page)->objectRects();
        bool chapterLink = false;
        for (const auto* object : objects) {
            if (object->objectType() != Okular::ObjectRect::Action)
                continue;
            const auto* action = static_cast<const Okular::Action*>(object->object());
            if (action->actionType() == Okular::Action::Goto) {
                const auto* go = static_cast<const Okular::GotoAction*>(action);
                if (go->destinationName().isEmpty())
                    QCOMPARE(go->destViewport().pageNumber, 1 - page); // Exported PDF keeps resolved actions.
                QVERIFY(go->fileName().isEmpty());
                document.setViewport(Okular::DocumentViewport(page));
                document.processAction(action);
                QCOMPARE(document.viewport().pageNumber, 1 - page);
                QVERIFY(!document.historyAtBegin());
                document.setPrevViewport();
                QCOMPARE(document.viewport().pageNumber, page);
                chapterLink = true;
            }
        }
        QVERIFY(chapterLink);
        const auto originalObjects = document.page(page)->objectRects();
        document.requestPixmaps(
            { new Okular::PixmapRequest(&observer, page, 700, 950, 1, 0, Okular::PixmapRequest::Asynchronous) });
        QTRY_VERIFY_WITH_TIMEOUT(document.page(page)->hasPixmap(&observer, 700, 950), 10000);
        QCOMPARE(document.page(page)->objectRects(), originalObjects);
    }

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_root.filePath("cache").toUtf8());
        qputenv("XDG_DATA_HOME", m_root.filePath("data").toUtf8());
        QCoreApplication::setLibraryPaths({ QStringLiteral(TEST_PLUGIN_ROOT) });
        qputenv("PATH", QByteArray(TEST_WORKER_DIR) + ':' + qgetenv("PATH"));
        Okular::SettingsCore::instance(QStringLiteral("epub-test"));
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup(config, QStringLiteral("OCR")).writeEntry("OcrTriggerMode", "Never");
        KConfigGroup(config, QStringLiteral("EPUB")).writeEntry("EpubFontSize", 11);
        KConfigGroup(config, QStringLiteral("EPUB")).writeEntry("EpubPageSize", "A5");
        // Keep asynchronous maintenance writes out of settings-change checks.
        KConfigGroup(config, QStringLiteral("Advanced"))
            .writeEntry("CacheLastVacuum", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        config->sync();
    }

    void printsPdfWithoutPostScriptConversion_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<bool>("preview");
        QTest::newRow("epub-preview") << QStringLiteral(TEST_EPUB_DIR "/sample.epub") << true;
        QTest::newRow("pdf-preview") << QStringLiteral(TEST_PDF_PATH) << true;
        QTest::newRow("epub-printer") << QStringLiteral(TEST_EPUB_DIR "/sample.epub") << false;
        QTest::newRow("pdf-printer") << QStringLiteral(TEST_PDF_PATH) << false;
    }

    void printsPdfWithoutPostScriptConversion()
    {
        QFETCH(QString, source);
        QFETCH(bool, preview);
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(source, QUrl::fromLocalFile(source), QMimeDatabase().mimeTypeForFile(source)),
                 Okular::Document::OpenSuccess);
        // Part::slotPrintPreview chooses a .pdf filename for NativePrinting.
        QCOMPARE(document.printingSupport(), Okular::Document::NativePrinting);
        const auto pageCount = document.pages();

        QTemporaryDir commands;
        QVERIFY(commands.isValid());
        const QString receipt = commands.filePath("receipt");
        const QString converted = commands.filePath("converted");
        QFile spooler(commands.filePath("lpr-cups"));
        QVERIFY(spooler.open(QIODevice::WriteOnly));
        // Capture the submitted PDF without sending anything to a real printer.
        const QByteArray spoolScript = "#!/bin/sh\nfor source do :; done\nIFS= read -r header < \"$source\"\n"
                                       "printf '%s\\n%s\\n' \"$header\" \"$source\" > '"
            + receipt.toUtf8() + "'\n";
        QCOMPARE(spooler.write(spoolScript), spoolScript.size());
        QVERIFY(spooler.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        spooler.close();
        QFile converter(commands.filePath("pdf2ps"));
        QVERIFY(converter.open(QIODevice::WriteOnly));
        const QByteArray conversionScript = "#!/bin/sh\nprintf called > '" + converted.toUtf8() + "'\nexit 42\n";
        QCOMPARE(converter.write(conversionScript), conversionScript.size());
        QVERIFY(converter.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        converter.close();
        const QByteArray oldPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
        qputenv("PATH", commands.path().toUtf8());

        QPrinter printer;
        QTemporaryFile output(commands.filePath("okular_XXXXXX.pdf"));
        if (preview) {
            // Match Okular's preview setup: the filename selects PDF output.
            QVERIFY(output.open());
            printer.setOutputFileName(output.fileName());
            output.close();
            QCOMPARE(printer.outputFormat(), QPrinter::PdfFormat);
        }
        printer.setPageOrientation(document.orientation());
        QCOMPARE(document.print(printer), Okular::Document::NoPrintError);
        QVERIFY(!QFile::exists(converted));

        QString printed;
        if (preview) {
            QVERIFY(!QFile::exists(receipt));
            printed = output.fileName();
        } else {
            QFile captured(receipt);
            QVERIFY(captured.open(QIODevice::ReadOnly));
            QVERIFY(captured.readLine().startsWith("%PDF-"));
            printed = QString::fromUtf8(captured.readLine()).trimmed();
        }
        // The mock spooler leaves its input behind; own cleanup on assertion failures too.
        const auto removeSpool = qScopeGuard([&] {
            if (!preview)
                QFile::remove(printed);
        });
        qputenv("PATH", oldPath);
        document.closeDocument();
        QCOMPARE(document.openDocument(printed,
                                       QUrl::fromLocalFile(printed),
                                       QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))),
                 Okular::Document::OpenSuccess);
        QCOMPARE(document.pages(), pageCount);
    }

    void firstTileHasStableNamedActions_data()
    {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<bool>("asynchronous");
        for (int rotation = 0; rotation < 4; ++rotation) {
            const auto name = QByteArray::number(rotation);
            QTest::newRow((name + "-async").constData()) << rotation << true;
            QTest::newRow((name + "-sync").constData()) << rotation << false;
        }
    }

    void firstTileHasStableNamedActions()
    {
        QFETCH(int, rotation);
        QFETCH(bool, asynchronous);
        const QString path = QStringLiteral(TEST_EPUB_DIR "/linked.epub");
        Okular::Document document(nullptr);
        Okular::DocumentObserver observer;
        document.addObserver(&observer);
        const auto cleanup = qScopeGuard([&] { document.removeObserver(&observer); });
        QCOMPARE(document.openDocument(path,
                                       QUrl::fromLocalFile(path),
                                       QMimeDatabase().mimeTypeForName(QStringLiteral("application/epub+zip"))),
                 Okular::Document::OpenSuccess);
        document.setRotation(rotation);
        Okular::NormalizedRect region(0, 0, 0.1, 0.1);
        bool hasTileManager = false;
        const auto render = [&](int width, int height) {
            auto* request = new Okular::PixmapRequest(&observer,
                                                      0,
                                                      width,
                                                      height,
                                                      1,
                                                      0,
                                                      asynchronous ? Okular::PixmapRequest::Asynchronous
                                                                   : Okular::PixmapRequest::NoFeature);
            // The initial large partial request creates Okular's tile manager;
            // subsequent requests use it explicitly, as PageView does.
            request->setTile(hasTileManager);
            request->setNormalizedRect(region);
            document.requestPixmaps({ request });
            hasTileManager = true;
        };
        render(4000, 6000);
        QTRY_VERIFY(document.page(0)->hasPixmap(&observer, 4000, 6000, region));
        const auto actions = document.page(0)->objectRects();
        QVERIFY(actions.size() >= 2); // A wrapped URL can contribute several rectangles.
        const Okular::Action* internal = nullptr;
        for (const auto* rect : actions) {
            if (rect->objectType() != Okular::ObjectRect::Action)
                continue;
            const auto* action = static_cast<const Okular::Action*>(rect->object());
            if (action->actionType() == Okular::Action::Goto) {
                const auto* go = static_cast<const Okular::GotoAction*>(action);
                QVERIFY(!go->destinationName().isEmpty());
                QVERIFY(go->fileName().isEmpty());
                internal = action;
            }
        }
        QVERIFY(internal);
        region = Okular::NormalizedRect(0.75, 0.75, 1, 1);
        render(4000, 6000);
        QTRY_VERIFY(document.page(0)->hasPixmap(&observer, 4000, 6000, region));
        QCOMPARE(document.page(0)->objectRects(), actions);
        document.processAction(internal);
        QCOMPARE(document.viewport().pageNumber, 2);
        Okular::GotoAction broken(QString(), QStringLiteral("nonexistent.html#missing"));
        document.processAction(&broken);
        QCOMPARE(document.viewport().pageNumber, 2);
    }

    void exportsEpubToAzw3()
    {
        const auto path = QStringLiteral(TEST_EPUB_DIR "/sample.epub");
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        const auto formats = document.exportFormats();
        const auto it = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
            return format.mimeType().name() == QStringLiteral("application/vnd.amazon.mobi8-ebook");
        });
        QVERIFY(it != formats.end());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto output = directory.filePath(QStringLiteral("export.azw3"));
        QVERIFY(document.exportTo(output, *it));
        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        const auto bytes = result.readAll();
        QCOMPARE(bytes.mid(60, 8), QByteArray("BOOKMOBI"));
    }

    void exportsKindleToEpub_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::newRow("azw3") << QStringLiteral("sample.azw3");
        QTest::newRow("kfx") << QStringLiteral("sample.kfx");
    }

    void exportsKindleToEpub()
    {
        QFETCH(QString, fixture);
        const QString path = QStringLiteral(TEST_MOBI_DIR "/") + fixture;
        Okular::Document document(nullptr);
        const auto kindleMime = QMimeDatabase().mimeTypeForName(QStringLiteral("application/vnd.amazon.mobi8-ebook"));
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), kindleMime), Okular::Document::OpenSuccess);
        const auto formats = document.exportFormats();
        const auto it = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
            return format.mimeType().name() == QStringLiteral("application/epub+zip");
        });
        QVERIFY(it != formats.end());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto output = directory.filePath(QStringLiteral("export.epub"));
        QVERIFY(document.exportTo(output, *it));
        QCOMPARE(QMimeDatabase().mimeTypeForFile(output, QMimeDatabase::MatchContent).name(),
                 QStringLiteral("application/epub+zip"));
    }

    void attachmentResultsAreCachedOnlyOnSuccess_data()
    {
        QTest::addColumn<int>("declaredSize");
        QTest::newRow("no-attachments") << 0;
        QTest::newRow("attachment") << 3;
        QTest::newRow("resource-limit") << 32 * 1024 * 1024;
    }

    void attachmentResultsAreCachedOnlyOnSuccess()
    {
        QFETCH(int, declaredSize);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("attachments.pdf"));
        auto* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        const auto cleanup = qScopeGuard([&] { fz_drop_context(context); });
        if (declaredSize)
            QVERIFY(createAttachmentPDF(context, path, declaredSize));
        else
            createTextPDF(context, path);

        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        QSignalSpy warnings(&document, &Okular::Document::warning);
        if (declaredSize > 3) {
            // Each failure must reach the worker again rather than cache an empty list.
            for (int attempt = 0; attempt < 2; ++attempt) {
                QVERIFY(!document.embeddedFiles());
                QTRY_COMPARE(warnings.count(), attempt + 1);
                QVERIFY(warnings.last().first().toString().contains(QStringLiteral("attachments exceed")));
            }
            return;
        }

        const auto* files = document.embeddedFiles();
        QVERIFY(files);
        QCOMPARE(files->size(), declaredSize ? 1 : 0);
        if (declaredSize)
            QCOMPARE(files->first()->data(), QByteArray("abc"));
        QCOMPARE(document.embeddedFiles(), files);
        QVERIFY(warnings.isEmpty());
    }

    void roundTrip_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::newRow("middle") << QStringLiteral("middle");
        QTest::newRow("top") << QStringLiteral("top");
    }

    void rootRelativeRoundTrip_data()
    {
        QTest::addColumn<int>("fontSize");
        QTest::newRow("small") << 10;
        QTest::newRow("large") << 20;
    }

    void rootRelativeRoundTrip()
    {
        if (!TEST_BUNDLED_MUPDF)
            QSKIP("Regression coverage for the bundled MuPDF rem patch");
        QFETCH(int, fontSize);
        const QString source = QStringLiteral(TEST_EPUB_DIR "/layout/rem.epub");
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup epub(config, QStringLiteral("EPUB"));
        epub.writeEntry("EpubFontSize", fontSize);
        epub.writeEntry("EpubPageSize", "Letter");
        config->sync();
        auto restore = qScopeGuard([&] {
            epub.writeEntry("EpubFontSize", 11);
            epub.writeEntry("EpubPageSize", "A5");
            config->sync();
            reloadPluginSettings();
        });
        reloadPluginSettings();
        QVERIFY(!QTest::currentTestFailed());
        Mu::Model::DocumentSettings settings;
        settings.epub.fontSize = fontSize;
        settings.epub.pageSize = Mu::Model::EpubPageSize::Letter;
        QVERIFY(!Mu::Plugin::Caching::EPUB::Cache::load(source, settings));
        Okular::DocumentObserver observer;
        Okular::Document document(nullptr);
        document.addObserver(&observer);
        auto cleanup = qScopeGuard([&] {
            document.closeDocument();
            document.removeObserver(&observer);
        });
        QList<Okular::TextEntity::List> coldWords;
        for (const bool warm : { false, true }) {
            QCOMPARE(document.openDocument(source,
                                           QUrl::fromLocalFile(source),
                                           QMimeDatabase().mimeTypeForName(QStringLiteral("application/epub+zip"))),
                     Okular::Document::OpenSuccess);
            QCOMPARE(document.pages(), 2U);
            QVERIFY(document.documentSynopsis());
            QCOMPARE(Mu::Test::synopsisCount(*document.documentSynopsis()), 2);
            for (int page = 0; page < 2; ++page) {
                document.requestTextPage(static_cast<unsigned int>(page));
                QTRY_VERIFY(document.page(page)->hasTextPage());
                document.requestPixmaps({ new Okular::PixmapRequest(
                    &observer, page, 600, 850, 1, 0, Okular::PixmapRequest::Asynchronous) });
                QTRY_VERIFY(document.page(page)->hasPixmap(&observer));
                const QString prefix = page == 0 ? QStringLiteral("One") : QStringLiteral("Two");
                QVERIFY(document.page(page)->text().contains(prefix + QStringLiteral("Paragraph")));
                const auto words =
                    document.page(page)->words(nullptr, Okular::TextPage::AnyPixelTextAreaInclusionBehaviour);
                if (!warm) {
                    coldWords.append(words);
                } else {
                    QCOMPARE(words.size(), coldWords[page].size());
                    for (int index = 0; index < words.size(); ++index) {
                        QCOMPARE(words[index].text(), coldWords[page][index].text());
                        QCOMPARE(words[index].area(), coldWords[page][index].area());
                    }
                }
                const auto hasChapterLink = [&] {
                    for (const auto* object : document.page(page)->objectRects()) {
                        if (object->objectType() != Okular::ObjectRect::Action)
                            continue;
                        const auto* action = static_cast<const Okular::Action*>(object->object());
                        if (action->actionType() == Okular::Action::Goto) {
                            const auto* go = static_cast<const Okular::GotoAction*>(action);
                            if (go->destinationName().isEmpty())
                                continue;
                            document.processAction(action);
                            return document.viewport().pageNumber == 1 - page;
                        }
                    }
                    return false;
                };
                QVERIFY(hasChapterLink());
            }
            const auto cache = Mu::Plugin::Caching::EPUB::Cache::load(source, settings);
            QVERIFY(cache && cache->accelerator && !cache->accelerator->isEmpty() && cache->outline);
            const QString output = m_root.filePath(QStringLiteral("rem-%1-%2.pdf").arg(fontSize).arg(warm));
            QSignalSpy notices(&document, &Okular::Document::notice);
            QVERIFY(document.exportTo(output, Okular::ExportFormat::standardFormat(Okular::ExportFormat::PDF)));
            const auto completed = [&] {
                return std::any_of(notices.cbegin(), notices.cend(), [](const auto& notice) {
                    return notice.front().toString().contains(QStringLiteral("Export to PDF finished"));
                });
            };
            QTRY_VERIFY_WITH_TIMEOUT(completed(), 20000);
            document.closeDocument();
            const auto inspection = inspectPdf(output);
            QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
            QVERIFY2(inspection.text.error.isEmpty(), qPrintable(inspection.text.error));
            for (const QString& prefix : { QStringLiteral("One"), QStringLiteral("Two") }) {
                const float root = static_cast<float>(fontSize) * (prefix == QStringLiteral("One") ? 0.85f : 1.25f);
                const QMap<QString, float> expected {
                    { "Heading", 1.35f * root }, { "Paragraph", root },    { "List", root },
                    { "Nestedrem", root },       { "Nestedem", 3 * root }, { "Fixed", 16 }
                };
                for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
                    const QString label = prefix + it.key();
                    QVERIFY2(inspection.text.lines.contains(label), qPrintable(label));
                    QVERIFY2(std::abs(inspection.text.lines[label].size - it.value()) < 0.02f, qPrintable(label));
                }
            }
            QCOMPARE(document.openDocument(output,
                                           QUrl::fromLocalFile(output),
                                           QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))),
                     Okular::Document::OpenSuccess);
            QCOMPARE(document.pages(), 2U);
            for (int page = 0; page < 2; ++page) {
                document.requestTextPage(static_cast<unsigned int>(page));
                QTRY_VERIFY(document.page(page)->hasTextPage());
                const QString prefix = page == 0 ? QStringLiteral("One") : QStringLiteral("Two");
                QVERIFY(document.page(page)->text().contains(prefix + QStringLiteral("Paragraph")));
            }
            document.closeDocument();
        }
    }

    void roundTrip()
    {
        QFETCH(QString, fixture);
        const QString source = QStringLiteral(TEST_EPUB_DIR "/layout/") + fixture + QStringLiteral(".epub");
        Mu::Model::DocumentSettings settings;
        settings.epub.pageSize = Mu::Model::EpubPageSize::A5;
        QVERIFY(!Mu::Plugin::Caching::EPUB::Cache::load(source, settings));
        Okular::DocumentObserver observer;
        Okular::Document document(nullptr);
        document.addObserver(&observer);
        const auto cleanup = qScopeGuard([&] {
            document.closeDocument();
            document.removeObserver(&observer);
        });
        QList<Okular::TextEntity::List> coldWords;
        for (const bool warm : { false, true }) {
            QCOMPARE(document.openDocument(source,
                                           QUrl::fromLocalFile(source),
                                           QMimeDatabase().mimeTypeForName(QStringLiteral("application/epub+zip"))),
                     Okular::Document::OpenSuccess);
            QCOMPARE(document.pages(), 2U);
            const auto* synopsis = document.documentSynopsis();
            QVERIFY(synopsis);
            QCOMPARE(Mu::Test::synopsisCount(*synopsis), 2);
            QCOMPARE(Mu::Test::firstSynopsisTitle(*synopsis),
                     warm ? QStringLiteral("Cached diagram chapter") : QStringLiteral("Diagram chapter"));
            for (int page = 0; page < 2; ++page) {
                checkPage(document, observer, page);
                QVERIFY(!QTest::currentTestFailed());
                const auto words =
                    document.page(page)->words(nullptr, Okular::TextPage::AnyPixelTextAreaInclusionBehaviour);
                if (!warm) {
                    coldWords.append(words);
                } else {
                    QCOMPARE(words.size(), coldWords[page].size());
                    for (int index = 0; index < words.size(); ++index) {
                        QCOMPARE(words[index].text(), coldWords[page][index].text());
                        QCOMPARE(words[index].area(), coldWords[page][index].area());
                    }
                }
            }
            const auto cache = Mu::Plugin::Caching::EPUB::Cache::load(source, settings);
            QVERIFY(cache && cache->accelerator && !cache->accelerator->isEmpty() && cache->outline);
            QCOMPARE(cache->outline->size(), std::size_t(2));
            QCOMPARE(cache->outline->front().title, std::string(warm ? "Cached diagram chapter" : "Diagram chapter"));
            QSignalSpy notices(&document, &Okular::Document::notice);
            const QString output = m_root.filePath(fixture + (warm ? "-warm.pdf" : "-cold.pdf"));
            QVERIFY(document.exportTo(output, Okular::ExportFormat::standardFormat(Okular::ExportFormat::PDF)));
            const auto completed = [&] {
                for (const auto& notice : notices)
                    if (notice.front().toString().contains(QStringLiteral("Export to PDF finished")))
                        return true;
                return false;
            };
            QTRY_VERIFY_WITH_TIMEOUT(completed(), 20000);
            document.closeDocument();
            const auto inspection = inspectPdf(output);
            QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
            QCOMPARE(inspection.images, 2);
            QVERIFY(inspection.embeddedFont);
            // The image alignment fix is carried by the bundled MuPDF patches.
            if (TEST_BUNDLED_MUPDF)
                QVERIFY(!inspection.overlappingText);
            QCOMPARE(document.openDocument(output,
                                           QUrl::fromLocalFile(output),
                                           QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))),
                     Okular::Document::OpenSuccess);
            QCOMPARE(document.pages(), 2U);
            QVERIFY(document.documentSynopsis());
            QCOMPARE(Mu::Test::synopsisCount(*document.documentSynopsis()), 2);
            QCOMPARE(Mu::Test::firstSynopsisTitle(*document.documentSynopsis()), QStringLiteral("Diagram chapter"));
            for (int page = 0; page < 2; ++page) {
                checkPage(document, observer, page);
                QVERIFY(!QTest::currentTestFailed());
                QString expected;
                for (const auto& word : coldWords[page])
                    expected += word.text();
                QCOMPARE(document.page(page)->text().remove(' ').remove('\n'), expected.remove(' ').remove('\n'));
            }
            document.closeDocument();
            if (!warm) {
                // A distinguishable title proves the plugin consumes the persisted outline on reopen.
                auto outline = *cache->outline;
                outline.front().title = "Cached diagram chapter";
                QVERIFY(Mu::Plugin::Caching::EPUB::Cache::saveOutline(source, settings, outline));
            }
        }
    }
};

QTEST_MAIN(TestGeneratorEpub)
#include "test_epub.moc"
