// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/epub_cache.hpp"
#include "support/synopsis.hpp"
#include "support/text_inspection.hpp"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDateTime>
#include <QFile>
#include <QMimeDatabase>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
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
        QTRY_VERIFY_WITH_TIMEOUT(linksReady(), 10000);
        const auto& objects = document.page(page)->objectRects();
        bool chapterLink = false;
        for (const auto* object : objects) {
            if (object->objectType() != Okular::ObjectRect::Action)
                continue;
            const auto* action = static_cast<const Okular::Action*>(object->object());
            if (action->actionType() == Okular::Action::Goto) {
                const auto* go = static_cast<const Okular::GotoAction*>(action);
                QCOMPARE(go->destViewport().pageNumber, 1 - page);
                chapterLink = true;
            }
        }
        QVERIFY(chapterLink);
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
                        if (action->actionType() == Okular::Action::Goto
                            && static_cast<const Okular::GotoAction*>(action)->destViewport().pageNumber == 1 - page)
                            return true;
                    }
                    return false;
                };
                QTRY_VERIFY(hasChapterLink());
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
