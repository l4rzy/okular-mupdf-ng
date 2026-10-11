// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/epub_cache.hpp"

#include <KConfigGroup>
#include <KSharedConfig>
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

namespace {

struct PdfInspection {
    int images = 0;
    bool embeddedFont = false;
    bool overlappingText = false;
    QString error;
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
        config->sync();
    }

    void roundTrip_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::newRow("middle") << QStringLiteral("middle");
        QTest::newRow("top") << QStringLiteral("top");
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
            QCOMPARE(synopsis->childNodes().size(), 2);
            QCOMPARE(synopsis->documentElement().tagName(),
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
            QCOMPARE(document.documentSynopsis()->childNodes().size(), 2);
            QCOMPARE(document.documentSynopsis()->documentElement().tagName(), QStringLiteral("Diagram chapter"));
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
