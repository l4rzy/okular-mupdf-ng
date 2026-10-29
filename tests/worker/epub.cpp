// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/constants.hpp"
#include "engine/epub/document.hpp"
#include "engine/epub/export_jobs.hpp"
#include "engine/pdf/document.hpp"
#include "runtime/command_service.hpp"
#include "shared/compat.hpp"
#include "shared/model/types.hpp"
#include "shared/transport/fd_channel.hpp"
#include "support/text_inspection.hpp"
#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <future>
#include <iterator>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct PathBoundsDevice {
    fz_device super;
    fz_rect bounds;
};

void collectPathBounds(fz_context* context,
                       fz_device* device,
                       const fz_path* path,
                       int,
                       fz_matrix transform,
                       fz_colorspace*,
                       const float*,
                       float,
                       fz_color_params)
{
    auto* collector = reinterpret_cast<PathBoundsDevice*>(device);
    collector->bounds = fz_union_rect(collector->bounds, fz_bound_path(context, path, nullptr, transform));
}

void verifyPdfFile(const QString& path)
{
    QFile pdfFile(path);
    QVERIFY(pdfFile.open(QIODevice::ReadOnly));
    const QByteArray content = pdfFile.read(32);
    pdfFile.close();
    QVERIFY2(content.startsWith("%PDF-"), content.constData());
}

/// Polls the export completion eventfd and drains notifications until at
/// least `count` arrive or the deadline passes. Returns whatever arrived.
std::vector<::Mu::Worker::Engine::ExportJobs::Notification>
waitForExportNotifications(::Mu::Worker::Engine::ExportJobs& jobs, std::size_t count)
{
    const int fd = jobs.eventFd();
    std::vector<::Mu::Worker::Engine::ExportJobs::Notification> result;
    for (int tick = 0; tick < 300 && result.size() < count && fd >= 0; ++tick) {
        pollfd pollSet { fd, POLLIN, 0 };
        ::poll(&pollSet, 1, 100);
        auto drained = jobs.drainNotifications();
        result.insert(result.end(), drained.begin(), drained.end());
    }
    return result;
}

std::vector<::Mu::Worker::Engine::ExportJobs::Notification>
waitForExportNotifications(::Mu::Worker::Runtime::CommandService& service, std::size_t count)
{
    const int fd = service.exportCompletionFd();
    std::vector<::Mu::Worker::Engine::ExportJobs::Notification> result;
    for (int tick = 0; tick < 300 && result.size() < count && fd >= 0; ++tick) {
        pollfd pollSet { fd, POLLIN, 0 };
        ::poll(&pollSet, 1, 100);
        auto drained = service.drainExportNotifications();
        result.insert(result.end(), drained.begin(), drained.end());
    }
    return result;
}

} // namespace

class TestEpub : public QObject {
    Q_OBJECT
private slots:

    void testAttributeSelectors_data()
    {
        QTest::addColumn<QString>("selector");
        QTest::addColumn<QString>("attributes");
        QTest::addColumn<bool>("matches");
        const auto add = [](const char* name, const QString& selector, const QString& attributes, bool matches) {
            QTest::newRow(name) << selector << attributes << matches;
        };
        add("invalid-prefix-operator", "[data-value^text]", "data-value='text'", false);
        add("invalid-suffix-value", "[data-value$=]", "data-value='text'", false);
        add("invalid-substring-bracket", "[data-value*='text'", "data-value='text'", false);
        add("invalid-operator", "[data-value**='text']", "data-value='text'", false);
        add("invalid-attribute-name", "[^='text']", "data-value='text'", false);
        add("long-suffix",
            "[data-value$='text']",
            QStringLiteral("data-value='%1text'").arg(QString(900, QLatin1Char('a'))),
            true);
        add("longer-dash-needle",
            QStringLiteral("[data-value|='%1']").arg(QString(1000, QLatin1Char('a'))),
            "data-value='a'",
            false);
        add("exists", "[data-value]", "data-value='text'", true);
        add("exists-empty", "[data-value]", "data-value=''", true);
        add("exists-missing", "[data-value]", "", false);
        add("exact", "[data-value='text']", "data-value='text'", true);
        add("exact-empty", "[data-value='']", "data-value=''", true);
        add("exact-missing", "[data-value='']", "", false);
        add("exact-case", "[data-value='text']", "data-value='Text'", false);
        add("exact-partial", "[data-value='text']", "data-value='textual'", false);
        for (const auto& op : { QStringLiteral("^"), QStringLiteral("$"), QStringLiteral("*") }) {
            const auto selector = QStringLiteral("[data-value%1='text']").arg(op);
            add(qPrintable(op + "-exact"), selector, "data-value='text'", true);
            add(qPrintable(op + "-missing"), selector, "", false);
            add(qPrintable(op + "-empty-attribute"), selector, "data-value=''", false);
            add(qPrintable(op + "-short"), selector, "data-value='tex'", false);
            add(qPrintable(op + "-case"), selector, "data-value='TEXT'", false);
            add(qPrintable(op + "-empty-needle"),
                QStringLiteral("[data-value%1='']").arg(op),
                "data-value='text'",
                false);
            add(qPrintable(op + "-both-empty"), QStringLiteral("[data-value%1='']").arg(op), "data-value=''", false);
        }
        add("operator-whitespace", "[ data-value ^= 'text' ]", "data-value='text-more'", true);
        add("exact-whitespace", "[ data-value = text ]", "data-value='text'", true);
        add("empty-whitespace", "[ data-value $= '' ]", "data-value='text'", false);
        add("prefix", "[data-value^=text]", "data-value='text-more'", true);
        add("prefix-middle", "[data-value^='text']", "data-value='a-text'", false);
        add("suffix", "[data-value$=text]", "data-value='more-text'", true);
        add("suffix-repeated", "[data-value$='text']", "data-value='text-more-text'", true);
        add("suffix-middle", "[data-value$='text']", "data-value='text-more'", false);
        add("substring", "[data-value*=text]", "data-value='a-text-b'", true);
        add("substring-miss", "[data-value*='text']", "data-value='next'", false);
        add("suffix-unicode", QString::fromUtf8("[data-value$='é']"), QString::fromUtf8("data-value='café'"), true);
        add("dash-exact", "[data-value|=en]", "data-value='en'", true);
        add("dash-prefix", "[data-value|=en]", "data-value='en-US'", true);
        add("dash-partial", "[data-value|=en]", "data-value='english'", false);
        add("dash-middle", "[data-value|=en]", "data-value='a-en'", false);
        add("dash-missing", "[data-value|='']", "", false);
        add("dash-empty", "[data-value|='']", "data-value=''", true);
        add("dash-empty-prefix", "[data-value|='']", "data-value='-en'", true);
        add("dash-short", "[data-value|=english]", "data-value='en'", false);
        add("dash-case", "[data-value|=en]", "data-value='EN-us'", false);
        add("dash-space", "[data-value|=en]", "data-value='en US'", false);
        add("token-missing", "[data-value~=text]", "", false);
        add("token-empty-value", "[data-value~=text]", "data-value=''", false);
        add("token-only-whitespace", "[data-value~=text]", "data-value=' &#9;&#13;&#10;&#12;'", false);
        add("token-trailing-whitespace", "[data-value~=text]", "data-value='other text &#9;'", true);
        add("token-nonbreaking-space", "[data-value~=text]", "data-value='other&#160;text'", false);
        add("token", "[data-value~=text]", "data-value='first text last'", true);
        add("token-repeat", "[data-value~=text]", "data-value='textual text'", true);
        add("token-whitespace", "[data-value~=text]", "data-value='first&#9;&#13;text&#10;&#12;last'", true);
        add("token-substring", "[data-value~=text]", "data-value='textual'", false);
        add("token-empty", "[data-value~='']", "data-value=''", false);
        add("token-space", "[data-value~='two words']", "data-value='two words'", false);
        add("class-token", ".text", "class='textual&#9;text'", true);
        add("compound", "p[data-value^='text'][data-value$='end']", "data-value='text-end'", true);
        add("compound-miss", "p[data-value^='text'][data-value$='end']", "data-value='text-other'", false);
        add("descendant", "div[data-parent*='book'] p[data-value$='marker']", "data-value='note-marker'", true);
        add("book-footnote", "p[data-type='footnote'] a[href$='-marker']", "href='note-marker'", true);
        add("book-footnote-miss", "p[data-type='footnote'] a[href$='-marker']", "href='note-marker-more'", false);
    }

    void testAttributeSelectors()
    {
#if TEST_SYSTEM_MUPDF
        QSKIP("Regression coverage for the bundled MuPDF attribute selector patch");
#endif
        QFETCH(QString, selector);
        QFETCH(QString, attributes);
        QFETCH(bool, matches);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const bool footnote = selector.contains(QStringLiteral("a[href"));
        const auto target = footnote ? QStringLiteral("<p data-type='footnote'><a %1>Target</a></p>").arg(attributes)
                                     : QStringLiteral("<p %1>Target</p>").arg(attributes);
        // The later type rule must lose to an attribute selector's specificity.
        // The control also checks that parsing continues after the tested selector.
        const auto content = QStringLiteral("<html><head><style>p { font-size: 12pt; } %1 { font-size: 24pt; } "
                                            "p { font-size: 12pt; } p.control { font-size: 18pt; } "
                                            "</style></head><body><div data-parent='my-book'>%2</div>"
                                            "<p class='control'>Control</p></body></html>")
                                 .arg(selector, target)
                                 .toUtf8();
        QFile html(directory.filePath(QStringLiteral("selectors.html")));
        QVERIFY(html.open(QIODevice::WriteOnly));
        QCOMPARE(html.write(content), content.size());
        html.close();
        ::Mu::Worker::Engine::EpubDocument owner;
        auto* context = owner.context();
        fz_document* document = nullptr;
        fz_var(document);
        QString error;
        const auto path = QFile::encodeName(html.fileName());
        fz_try(context)
        {
            document = fz_open_document(context, path.constData());
            fz_layout_document(context, document, 400, 600, 12);
        }
        fz_catch(context)
        {
            error = QString::fromUtf8(fz_caught_message(context));
        }
        const auto cleanup = qScopeGuard([&] { fz_drop_document(context, document); });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto inspection = Mu::Test::inspectDocumentText(context, document);
        QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
        QVERIFY(inspection.lines.contains(QStringLiteral("Target")));
        QVERIFY(inspection.lines.contains(QStringLiteral("Control")));
        QCOMPARE(inspection.lines.value(QStringLiteral("Target")).size, matches ? 24.0f : 12.0f);
        QCOMPARE(inspection.lines.value(QStringLiteral("Control")).size, 18.0f);
    }

    void testTableBackground_data()
    {
        QTest::addColumn<QString>("background");
        QTest::addColumn<int>("expectedRgb");
        QTest::newRow("none") << QStringLiteral("background: none;") << 0xd0ccb8;
        QTest::newRow("none-uppercase") << QStringLiteral("background: NONE;") << 0xd0ccb8;
        QTest::newRow("none-mixed-case") << QStringLiteral("background: NoNe;") << 0xd0ccb8;
        QTest::newRow("none-comment") << QStringLiteral("background: /* reset */ none;") << 0xd0ccb8;
        QTest::newRow("none-compact-important") << QStringLiteral("background:none!important;") << 0xd0ccb8;
        QTest::newRow("none-important-wins")
            << QStringLiteral("background: none !important; background: black;") << 0xd0ccb8;
        QTest::newRow("earlier-color-important-wins")
            << QStringLiteral("background-color: #336699 !important; background: none;") << 0x336699;
        QTest::newRow("later-color-wins") << QStringLiteral("background: none; background: #336699;") << 0x336699;
        QTest::newRow("none-important") << QStringLiteral("background: none !important;") << 0xd0ccb8;
        QTest::newRow("reset-color") << QStringLiteral("background-color: red; background: none;") << 0xd0ccb8;
        QTest::newRow("transparent") << QStringLiteral("background: transparent;") << 0xd0ccb8;
        QTest::newRow("hex-color") << QStringLiteral("background: #336699;") << 0x336699;
        QTest::newRow("named-color") << QStringLiteral("background: black;") << 0x000000;
    }

    void testTableBackground()
    {
#if TEST_SYSTEM_MUPDF
        QSKIP("Regression coverage for the bundled MuPDF background shorthand patch");
#endif
        QFETCH(QString, background);
        QFETCH(int, expectedRgb);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // Match the book's table > cell > div > pre structure, with a visible parent background.
        const auto content =
            QStringLiteral("<html><head><style>@page { margin: 0; } body { margin: 0; background: #d0ccb8; } "
                           "table { width: 200px; border-spacing: 0; %1 } td { padding: 0; } pre { margin: 0; } "
                           "</style></head><body><table><tr><td><div><pre>spine01\ninterface lo\nrouter bgp 65000\n"
                           "network 10.0.0.21/32</pre></div></td></tr></table></body></html>")
                .arg(background)
                .toUtf8();
        QFile html(directory.filePath(QStringLiteral("table.html")));
        QVERIFY(html.open(QIODevice::WriteOnly));
        QCOMPARE(html.write(content), content.size());
        html.close();

        ::Mu::Worker::Engine::EpubDocument owner;
        auto* context = owner.context();
        fz_document* document = nullptr;
        fz_pixmap* pixmap = nullptr;
        fz_var(document);
        fz_var(pixmap);
        QString error;
        const auto path = QFile::encodeName(html.fileName());
        fz_try(context)
        {
            document = fz_open_document(context, path.constData());
            fz_layout_document(context, document, 300, 200, 12);
            pixmap = fz_new_pixmap_from_page_number(context, document, 0, fz_identity, fz_device_rgb(context), 0);
        }
        fz_catch(context)
        {
            error = QString::fromUtf8(fz_caught_message(context));
        }
        const auto cleanup = qScopeGuard([&] {
            fz_drop_pixmap(context, pixmap);
            fz_drop_document(context, document);
        });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto* pixel = fz_pixmap_samples(context, pixmap) + 25 * fz_pixmap_stride(context, pixmap)
            + 180 * fz_pixmap_components(context, pixmap);
        QCOMPARE((int(pixel[0]) << 16) | (int(pixel[1]) << 8) | int(pixel[2]), expectedRgb);
        const auto inspection = Mu::Test::inspectDocumentText(context, document);
        QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
        QVERIFY(inspection.lines.contains(QStringLiteral("spine01")));
        QVERIFY(inspection.lines.contains(QStringLiteral("router bgp 65000")));
    }

    void testWideImagesFitPage_data()
    {
        QTest::addColumn<QString>("display");
        QTest::addColumn<QString>("dimensions");
        QTest::addColumn<bool>("small");
        for (const auto& display : { QStringLiteral("inline"), QStringLiteral("block") }) {
            QTest::newRow(qPrintable(display + "-wide"))
                << display << QStringLiteral("width='902' height='484'") << false;
            QTest::newRow(qPrintable(display + "-intrinsic")) << display << QString() << false;
            QTest::newRow(qPrintable(display + "-small"))
                << display << QStringLiteral("width='90' height='48'") << true;
        }
    }

    void testWideImagesFitPage()
    {
        QFETCH(QString, display);
#if TEST_SYSTEM_MUPDF
        if (display == QStringLiteral("block"))
            QSKIP("Regression coverage for the bundled MuPDF block image patch");
#endif
        QFETCH(QString, dimensions);
        QFETCH(bool, small);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage image(902, 484, QImage::Format_RGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        QFile file(directory.filePath(QStringLiteral("image.html")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto html = QStringLiteral("<html><head><style>img { display: %1; width: auto !important; "
                                         "height: auto !important; max-width: 100%; background: lime; } "
                                         "p { margin: 0; line-height: 16pt; }</style></head>"
                                         "<body><ol><li><figure><img %2 "
                                         "src='data:image/png;base64,%3'/><figcaption><p>Caption</p></figcaption>"
                                         "</figure></li></ol><p>Following</p></body></html>")
                              .arg(display, dimensions, QString::fromLatin1(png.toBase64()))
                              .toUtf8();
        QCOMPARE(file.write(html), html.size());
        file.close();
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument doc;
        std::string error;
        QVERIFY2(doc.openFd(::dup(file.handle()), "image.html", &error), error.c_str());
        const auto inspection = ::Mu::Test::inspectDocumentText(doc.context(), doc.document());
        QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
        QCOMPARE(inspection.images.size(), 1);
        const auto geometry = doc.pageGeometry(0, &error);
        const auto bounds = inspection.images.front();
        QVERIFY2(
            bounds.x1 <= geometry.widthPoints + 0.01,
            qPrintable(
                QStringLiteral("Image right edge %1 exceeds page width %2").arg(bounds.x1).arg(geometry.widthPoints)));
        QVERIFY(bounds.x0 >= 0);
        const float width = bounds.x1 - bounds.x0;
        const float height = bounds.y1 - bounds.y0;
        QVERIFY(std::abs(width / height - (small ? 90.0f / 48.0f : 902.0f / 484.0f)) < 0.01f);
        if (small)
            QVERIFY(width < geometry.widthPoints / 2);
        QCOMPARE(doc.pageCount(), 1);
        QVERIFY(inspection.lines.contains(QStringLiteral("Caption")));
        QVERIFY(inspection.lines.contains(QStringLiteral("Following")));
        const float captionY = inspection.lines.value(QStringLiteral("Caption")).origin.y;
        QVERIFY(captionY >= bounds.y1);
        QVERIFY2(captionY - bounds.y1 < 32, qPrintable(QStringLiteral("Caption gap: %1pt").arg(captionY - bounds.y1)));
        QVERIFY(inspection.lines.value(QStringLiteral("Following")).origin.y > captionY);

        // Inspect drawing commands before clipping so overflowing block backgrounds remain visible to the test.
        auto* device =
            reinterpret_cast<PathBoundsDevice*>(fz_new_device_of_size(doc.context(), sizeof(PathBoundsDevice)));
        device->bounds = fz_empty_rect;
        device->super.fill_path = collectPathBounds;
        auto cleanup = qScopeGuard([&] { fz_drop_device(doc.context(), &device->super); });
        fz_try(doc.context())
        {
            fz_page* page = fz_load_page(doc.context(), doc.document(), 0);
            fz_try(doc.context())
            {
                fz_run_page(doc.context(), page, &device->super, fz_identity, nullptr);
                fz_close_device(doc.context(), &device->super);
            }
            fz_always(doc.context())
            {
                fz_drop_page(doc.context(), page);
            }
            fz_catch(doc.context())
            {
                fz_rethrow(doc.context());
            }
        }
        fz_catch(doc.context())
        {
            QFAIL(fz_caught_message(doc.context()));
        }
        QVERIFY2(device->bounds.x1 <= geometry.widthPoints + 0.01,
                 qPrintable(QStringLiteral("Block right edge: %1pt").arg(device->bounds.x1)));
    }

    void testIndependentContextsShareLocks_data()
    {
        QTest::addColumn<int>("lock");
        QTest::newRow("allocator") << int(FZ_LOCK_ALLOC);
        QTest::newRow("freetype-and-harfbuzz") << int(FZ_LOCK_FREETYPE);
        QTest::newRow("glyph-cache") << int(FZ_LOCK_GLYPHCACHE);
    }

    void testIndependentContextsShareLocks()
    {
        QFETCH(int, lock);
        ::Mu::Worker::Engine::PdfDocument pdf;
        ::Mu::Worker::Engine::EpubDocument epub;
        QVERIFY(pdf.context());
        QVERIFY(epub.context());
        fz_lock(pdf.context(), lock);
        std::promise<void> attempting;
        auto started = attempting.get_future();
        auto worker = std::async(std::launch::async, [&] {
            attempting.set_value();
            fz_lock(epub.context(), lock);
            fz_unlock(epub.context(), lock);
        });
        started.wait();
        const auto blocked = worker.wait_for(std::chrono::milliseconds(30));
        // Always release and join before asserting, including the failure path.
        fz_unlock(pdf.context(), lock);
        worker.get();
        QCOMPARE(blocked, std::future_status::timeout);
    }

    void testStylesheetMedia_data()
    {
        QTest::addColumn<QString>("media");
        QTest::addColumn<bool>("linked");
        QTest::addColumn<bool>("applies");
        const std::array cases {
            std::pair { "", true },
            std::pair { "media=''", true },
            std::pair { "media='screen'", true },
            std::pair { "media='   '", true },
            std::pair { "media=', print, '", false },
            std::pair { "media='all'", true },
            std::pair { "media='print'", false },
            std::pair { "media='speech'", false },
            std::pair { "media='print, speech'", false },
            std::pair { "media='print, screen'", true },
            std::pair { "media='print, all'", true },
            std::pair { "media='  ScReEn  '", true },
            std::pair { "media='  PRINT  '", false },
            // Preserve existing handling of queries outside simple media lists.
            std::pair { "media='screen and (min-width: 1px)'", true },
        };
        for (const bool linked : { false, true }) {
            for (const auto& [media, applies] : cases) {
                const auto name = QStringLiteral("%1:%2").arg(linked ? "link" : "style", media).toUtf8();
                QTest::newRow(name.constData()) << QString::fromLatin1(media) << linked << applies;
            }
        }
    }

    void testStylesheetMedia()
    {
#if TEST_SYSTEM_MUPDF
        QSKIP("Regression coverage for the bundled MuPDF stylesheet media patch");
#endif
        QFETCH(QString, media);
        QFETCH(bool, linked);
        QFETCH(bool, applies);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray css = ".noprint { display: none }";
        QFile stylesheet(directory.filePath(QStringLiteral("style.css")));
        QVERIFY(stylesheet.open(QIODevice::WriteOnly));
        QCOMPARE(stylesheet.write(css), css.size());
        stylesheet.close();
        const auto style = linked
            ? QStringLiteral("<link rel='stylesheet' type='text/css' href='style.css' %1>").arg(media)
            : QStringLiteral("<style type='text/css' %1>%2</style>").arg(media, QString::fromLatin1(css));
        const auto content = QStringLiteral("<html><head>%1</head><body class='noprint'>"
                                            "<p>Visible</p></body></html>")
                                 .arg(style)
                                 .toUtf8();
        QFile html(directory.filePath(QStringLiteral("media.html")));
        QVERIFY(html.open(QIODevice::WriteOnly));
        QCOMPARE(html.write(content), content.size());
        html.close();

        ::Mu::Worker::Engine::EpubDocument owner;
        auto* context = owner.context();
        fz_document* document = nullptr;
        fz_var(document);
        QString error;
        const auto path = QFile::encodeName(html.fileName());
        fz_try(context)
        {
            document = fz_open_document(context, path.constData());
            fz_layout_document(context, document, 400, 600, 12);
        }
        fz_catch(context)
        {
            error = QString::fromUtf8(fz_caught_message(context));
        }
        const auto cleanup = qScopeGuard([&] { fz_drop_document(context, document); });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto inspection = Mu::Test::inspectDocumentText(context, document);
        QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
        QCOMPARE(inspection.lines.contains(QStringLiteral("Visible")), !applies);
    }

    void testRootRelativeLengths_data()
    {
        QTest::addColumn<QString>("rootStyle");
        QTest::addColumn<float>("rootScale");
        QTest::addColumn<float>("rootFixed");
        QTest::newRow("default") << QString() << 1.0f << 0.0f;
        QTest::newRow("percent") << QStringLiteral("font-size: 85%;") << 0.85f << 0.0f;
        QTest::newRow("em") << QStringLiteral("font-size: 1.25em;") << 1.25f << 0.0f;
        QTest::newRow("root-rem") << QStringLiteral("font-size: 2rem;") << 2.0f << 0.0f;
        QTest::newRow("fixed") << QStringLiteral("font-size: 18pt;") << 0.0f << 18.0f;
    }

    void testRootRelativeLengths()
    {
#if TEST_SYSTEM_MUPDF
        QSKIP("Regression coverage for the bundled MuPDF rem patch");
#endif
        QFETCH(QString, rootStyle);
        QFETCH(float, rootScale);
        QFETCH(float, rootFixed);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        QFile html(directory.filePath(QStringLiteral("rem.html")));
        QVERIFY(html.open(QIODevice::WriteOnly));
        const auto content =
            QStringLiteral("<html><head><style>@page { margin: 1rem; } html { %1 }"
                           "body { margin: 0; } p, h2 { margin: 0; line-height: 2rem; }"
                           "p { font-size: 1rem; } h2 { font-size: 1.35rem; }"
                           "ul { margin: 0; padding: 0; list-style: none; }"
                           ".offset { margin-left: 2rem; padding-left: 1rem; border-left: .5rem solid black; }"
                           "</style></head><body><p>Paragraph</p><p>Next</p><h2>Heading</h2><ul><li>List</li></ul>"
                           "<div style='font-size: 2em'><p>Nestedrem</p>"
                           "<p style='font-size: 1.5em'>Nestedem</p>"
                           "<p style='font-size: 50%'>Percent</p></div>"
                           "<p style='font-size: 16pt'>Fixed</p><p class='offset'>Offset</p>"
                           "<p style='text-indent: 2rem'>Indent</p>"
                           "<div style='font-size: 2em; margin-left: 1rem'>Inherited</div>"
                           "<p><img style='width: 2rem; height: 1rem' src='data:image/png;base64,%2'/></p>"
                           "<table style='border-spacing: 1rem'><tr>"
                           "<td style='padding: 0; height: 3rem; font-size: 1pt; line-height: 1pt'>CellOne</td></tr>"
                           "<tr><td style='padding: 0; height: 3rem; font-size: 1pt; line-height: "
                           "1pt'>CellTwo</td></tr></table>"
                           "<table style='border-spacing: 1rem'><tr style='height: 4rem'>"
                           "<td style='padding: 0; font-size: 1pt; line-height: 1pt'>RowOne</td></tr>"
                           "<tr style='height: 4rem'><td style='padding: 0; font-size: 1pt; line-height: "
                           "1pt'>RowTwo</td></tr></table>"
                           "</body></html>")
                .arg(rootStyle)
                .arg(QString::fromLatin1(png.toBase64()))
                .toUtf8();
        QCOMPARE(html.write(content), content.size());
        html.close();

        ::Mu::Worker::Engine::EpubDocument owner;
        auto* context = owner.context();
        fz_document* document = nullptr;
        fz_var(document);
        QString error;
        const auto path = QFile::encodeName(html.fileName());
        fz_try(context)
        {
            document = fz_open_document(context, path.constData());
        }
        fz_catch(context)
        {
            error = QString::fromUtf8(fz_caught_message(context));
        }
        const auto cleanup = qScopeGuard([&] { fz_drop_document(context, document); });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        for (const float base : { 10.0f, 20.0f, 10.0f }) {
            fz_try(context)
            {
                fz_layout_document(context, document, 1000, 4000, base);
            }
            fz_catch(context)
            {
                error = QString::fromUtf8(fz_caught_message(context));
            }
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const auto inspection = Mu::Test::inspectDocumentText(context, document);
            QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
            const float root = rootScale * base + rootFixed;
            const QMap<QString, float> expected { { "Paragraph", root },    { "Heading", 1.35f * root },
                                                  { "List", root },         { "Nestedrem", root },
                                                  { "Nestedem", 3 * root }, { "Percent", root },
                                                  { "Fixed", 16 },          { "Inherited", 2 * root } };
            for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
                QVERIFY2(inspection.lines.contains(it.key()), qPrintable(it.key()));
                QVERIFY2(std::abs(inspection.lines[it.key()].size - it.value()) < 0.01f, qPrintable(it.key()));
            }
            const auto& lines = inspection.lines;
            QVERIFY(lines.contains("Next") && lines.contains("Offset") && lines.contains("Indent"));
            QVERIFY(std::abs(lines["Paragraph"].origin.x - root) < 0.01f);
            QVERIFY(std::abs(lines["Next"].origin.y - lines["Paragraph"].origin.y - 2 * root) < 0.01f);
            QVERIFY(std::abs(lines["Offset"].origin.x - lines["Paragraph"].origin.x - 3.5f * root) < 0.01f);
            QVERIFY(std::abs(lines["Indent"].origin.x - lines["Paragraph"].origin.x - 2 * root) < 0.01f);
            QVERIFY(std::abs(lines["Inherited"].origin.x - lines["Paragraph"].origin.x - root) < 0.01f);
            QCOMPARE(inspection.images.size(), 1);
            const auto imageBounds = inspection.images.front();
            QVERIFY(std::abs(imageBounds.x1 - imageBounds.x0 - 2 * root) < 0.01f);
            QVERIFY(std::abs(imageBounds.y1 - imageBounds.y0 - root) < 0.01f);
            QVERIFY(lines.contains("CellOne") && lines.contains("CellTwo"));
            QVERIFY(std::abs(lines["CellTwo"].origin.y - lines["CellOne"].origin.y - 4 * root) < 0.01f);
            QVERIFY(lines.contains("RowOne") && lines.contains("RowTwo"));
            QVERIFY(std::abs(lines["RowTwo"].origin.y - lines["RowOne"].origin.y - 5 * root) < 0.01f);
        }
    }

    void testInlineRootRelativeFontSize()
    {
#if TEST_SYSTEM_MUPDF
        QSKIP("Regression coverage for the bundled MuPDF rem patch");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile html(directory.filePath(QStringLiteral("inline.html")));
        QVERIFY(html.open(QIODevice::WriteOnly));
        const QByteArray content("<html style='display:inline; font-size:2rem'>"
                                 "<body style='display:inline'>Inline</body></html>");
        QCOMPARE(html.write(content), content.size());
        html.close();
        ::Mu::Worker::Engine::EpubDocument owner;
        auto* context = owner.context();
        fz_document* document = nullptr;
        fz_var(document);
        QString error;
        const auto path = QFile::encodeName(html.fileName());
        fz_try(context)
        {
            document = fz_open_document(context, path.constData());
        }
        fz_catch(context)
        {
            error = QString::fromUtf8(fz_caught_message(context));
        }
        const auto cleanup = qScopeGuard([&] { fz_drop_document(context, document); });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        for (const float base : { 10.0f, 20.0f, 10.0f }) {
            fz_try(context)
            {
                fz_layout_document(context, document, 1000, 1000, base);
            }
            fz_catch(context)
            {
                error = QString::fromUtf8(fz_caught_message(context));
            }
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const auto inspection = Mu::Test::inspectDocumentText(context, document);
            QVERIFY2(inspection.error.isEmpty(), qPrintable(inspection.error));
            QVERIFY(inspection.lines.contains("Inline"));
            QVERIFY(std::abs(inspection.lines["Inline"].size - 2 * base) < 0.01f);
        }
    }

    void testStoryRootRelativeLengths_data() { testRootRelativeLengths_data(); }

    void testStoryRootRelativeLengths()
    {
#if TEST_SYSTEM_MUPDF
        QSKIP("Regression coverage for the bundled MuPDF rem patch");
#endif
        QFETCH(QString, rootStyle);
        QFETCH(float, rootScale);
        QFETCH(float, rootFixed);
        const auto content =
            QStringLiteral("<html><head><style>html { %1 } body { margin: 0; }"
                           "p { margin: 0; font-size: 1rem; line-height: 2rem; }</style></head>"
                           "<body><p>A</p><p>B</p><p>C</p><p>D</p><p>E</p><p>F</p><p>G</p><p>H</p></body></html>")
                .arg(rootStyle)
                .toUtf8();
        ::Mu::Worker::Engine::EpubDocument owner;
        auto* context = owner.context();
        for (const float base : { 10.0f, 20.0f }) {
            fz_buffer* buffer = nullptr;
            fz_story* story = nullptr;
            fz_display_list* list = nullptr;
            fz_device* device = nullptr;
            fz_stext_page* text = nullptr;
            int characters = 0;
            int placements = 0;
            bool correctSize = true;
            QString error;
            fz_var(buffer);
            fz_var(story);
            fz_var(list);
            fz_var(device);
            fz_var(text);
            fz_var(characters);
            fz_var(placements);
            fz_var(correctSize);
            const float root = rootScale * base + rootFixed;
            const fz_rect bounds { 0, 0, 1000, 4 * root + 0.1f };
            fz_try(context)
            {
                buffer = fz_new_buffer_from_copied_data(context,
                                                        reinterpret_cast<const unsigned char*>(content.constData()),
                                                        static_cast<std::size_t>(content.size()));
                story = fz_new_story(context, buffer, nullptr, base, nullptr);
                int more = 1;
                while (more && placements < 8) {
                    fz_rect filled;
                    more = fz_place_story(context, story, bounds, &filled);
                    list = fz_new_display_list(context, bounds);
                    device = fz_new_list_device(context, list);
                    fz_draw_story(context, story, device, fz_identity);
                    fz_close_device(context, device);
                    fz_drop_device(context, device);
                    device = nullptr;
                    text = fz_new_stext_page_from_display_list(context, list, nullptr);
                    for (auto* block = text->first_block; block; block = block->next) {
                        if (block->type != FZ_STEXT_BLOCK_TEXT)
                            continue;
                        for (auto* line = block->u.t.first_line; line; line = line->next) {
                            for (auto* ch = line->first_char; ch; ch = ch->next) {
                                ++characters;
                                correctSize = correctSize && std::abs(ch->size - root) < 0.01f;
                            }
                        }
                    }
                    fz_drop_stext_page(context, text);
                    text = nullptr;
                    fz_drop_display_list(context, list);
                    list = nullptr;
                    ++placements;
                }
            }
            fz_always(context)
            {
                fz_drop_stext_page(context, text);
                fz_drop_device(context, device);
                fz_drop_display_list(context, list);
                fz_drop_story(context, story);
                fz_drop_buffer(context, buffer);
            }
            fz_catch(context)
            {
                error = QString::fromUtf8(fz_caught_message(context));
            }
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(characters, 8);
            QVERIFY(placements > 1 && placements < 8);
            QVERIFY(correctSize);
        }
    }

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

    void testPageCacheReuseAndLifecycle()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "sample.epub", &error), error.c_str());
        QVERIFY(!document.isPageCached(0));

        std::vector<std::uint8_t> first(400U * 300U * 4U);
        std::vector<std::uint8_t> second(400U * 300U * 4U);
        const ::Mu::Worker::Engine::DocumentBase::RenderRequest request { 0, 400, 300, std::nullopt };
        QVERIFY2(document.renderToBuffer(request, first.data(), 400U * 4U, &error), error.c_str());
        QVERIFY(document.isPageCached(0));
        QVERIFY2(document.renderToBuffer(request, second.data(), 400U * 4U, &error), error.c_str());
        QVERIFY(document.isPageCached(0));
        QCOMPARE(second, first);

        // Rendering settings changes must not evict the parsed page.
        ::Mu::Model::DocumentSettings settings;
        settings.paperColorRgb = 0x112233;
        document.setSettings(settings);
        QVERIFY(document.isPageCached(0));

        document.close();
        QVERIFY(!document.isPageCached(0));
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

    void testPageMargins_data()
    {
        QTest::addColumn<QString>("publisherCss");
        QTest::addColumn<QByteArray>("customCss");
        QTest::addColumn<double>("pageMargin");
        QTest::newRow("default") << QString() << QByteArray() << 0.0;
        QTest::newRow("zero") << QStringLiteral("@page { margin: 0; }") << QByteArray() << 0.0;
        QTest::newRow("positive") << QStringLiteral("@page { margin: 24pt; }") << QByteArray() << 0.0;
        QTest::newRow("negative") << QStringLiteral("@page { margin: -20em 4em 1em 2em; }") << QByteArray() << 0.0;
        QTest::newRow("publisher-important")
            << QStringLiteral("@page { margin: -20em !important; }") << QByteArray() << 0.0;
        QTest::newRow("custom") << QStringLiteral("@page { margin: -20em; }")
                                << QByteArrayLiteral("@page { margin: 24pt !important; }") << 24.0;
    }

    void testPageMargins()
    {
        QFETCH(QString, publisherCss);
        QFETCH(QByteArray, customCss);
        QFETCH(double, pageMargin);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("margins.html")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QString content = QStringLiteral("<html><head><style>%1 p { margin: 0; line-height: 24pt; }</style>"
                                         "</head><body>")
                              .arg(publisherCss);
        constexpr int lineCount = 100;
        for (int line = 0; line < lineCount; ++line)
            content += QStringLiteral("<p>Line%1</p>").arg(line);
        content += QStringLiteral("</body></html>");
        const auto bytes = content.toUtf8();
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        QVERIFY(file.open(QIODevice::ReadOnly));

        ::Mu::Worker::Engine::EpubDocument doc;
        ::Mu::Model::DocumentSettings settings;
        settings.epub.customCssBase64 = customCss.toBase64().toStdString();
        doc.setSettings(settings);
        std::string error;
        QVERIFY2(doc.openFd(::dup(file.handle()), "margins.html", &error), error.c_str());
        QVERIFY(doc.pageCount() > 1);

        QStringList lines;
        for (int page = 0; page < doc.pageCount(); ++page) {
            const auto geometry = doc.pageGeometry(page, &error);
            const auto boxes = doc.textBoxes(page, 72, 72, 10000, false, &error);
            QVERIFY2(error.empty(), error.c_str());
            const double marginX = customCss.isEmpty()
                ? geometry.widthPoints * ::Mu::Worker::Engine::Constant::PageMarginFraction
                : pageMargin;
            const double marginY = customCss.isEmpty()
                ? geometry.heightPoints * ::Mu::Worker::Engine::Constant::PageMarginFraction
                : pageMargin;
            QString line;
            for (const auto& box : boxes) {
                QVERIFY(box.left >= marginX - 0.01);
                QVERIFY(box.top >= marginY - 0.01);
                QVERIFY(box.right <= geometry.widthPoints - marginX + 0.01);
                QVERIFY(box.bottom <= geometry.heightPoints - marginY + 0.01);
                line += QString::fromStdString(box.text);
                if (box.endOfLine) {
                    lines.append(line);
                    line.clear();
                }
            }
            QVERIFY(line.isEmpty());
            if (!boxes.empty()) {
                QVERIFY(std::abs(boxes.front().left - marginX) < 0.1);
            }
        }
        QCOMPARE(lines.size(), lineCount);
        for (int line = 0; line < lineCount; ++line)
            QCOMPARE(lines[line], QStringLiteral("Line%1").arg(line));
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

    void testImageVerticalAlignment_data()
    {
        QTest::addColumn<QString>("alignment");
        QTest::addColumn<int>("imageHeight");
        QTest::addColumn<bool>("inlineText");
        QTest::addColumn<bool>("nearPageEnd");
        QTest::addColumn<bool>("secondImage");
        for (const auto* alignment : { "baseline",
                                       "middle",
                                       "text-top",
                                       "text-bottom",
                                       "top",
                                       "bottom",
                                       "sub",
                                       "super",
                                       "25%",
                                       "-25%",
                                       "18pt",
                                       "-18pt" }) {
            for (const int height : { 6, 80, 180 }) {
                for (const bool inlineText : { false, true }) {
                    for (const bool nearPageEnd : { false, true }) {
                        const QByteArray name = QByteArray(alignment) + '-' + QByteArray::number(height)
                            + (inlineText ? "-inline" : "-alone") + (nearPageEnd ? "-page-end" : "-page-start");
                        QTest::newRow(name.constData())
                            << QString::fromLatin1(alignment) << height << inlineText << nearPageEnd << false;
                        QTest::newRow((name + "-mixed-images").constData())
                            << QString::fromLatin1(alignment) << height << inlineText << nearPageEnd << true;
                    }
                }
            }
        }
    }

    void testImageVerticalAlignment()
    {
#if TEST_SYSTEM_MUPDF
        QSKIP("Regression coverage for the bundled MuPDF image alignment patch");
#endif
        QFETCH(QString, alignment);
        QFETCH(int, imageHeight);
        QFETCH(bool, inlineText);
        QFETCH(bool, nearPageEnd);
        QFETCH(bool, secondImage);

        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile html(directory.filePath(QStringLiteral("alignment.html")));
        QVERIFY(html.open(QIODevice::WriteOnly));
        const QByteArray content =
            QStringLiteral("<html><head><style>@page { margin: 12pt; } body { margin: 0; font-size: 12pt; }"
                           "p { margin: 0; line-height: 2; } img { width: 40pt; height: %1pt; vertical-align: %2; }"
                           "</style></head><body><p>Before</p><div style='height: %3pt'></div>"
                           "<figure style='margin: 0'><img src='data:image/png;base64,%4'/>%5%6"
                           "<figcaption><p>After</p></figcaption></figure></body></html>")
                .arg(imageHeight)
                .arg(alignment)
                .arg(nearPageEnd ? 150 : 0)
                .arg(QString::fromLatin1(png.toBase64()))
                .arg(inlineText ? QStringLiteral("Inline") : QString())
                .arg(secondImage ? QStringLiteral("<img style='height: 24pt; vertical-align: baseline' "
                                                  "src='data:image/png;base64,%1'/>")
                                       .arg(QString::fromLatin1(png.toBase64()))
                                 : QString())
                .toUtf8();
        QCOMPARE(html.write(content), content.size());
        html.close();

        // Open HTML directly so plugin CSS cannot mask a MuPDF layout regression.
        ::Mu::Worker::Engine::EpubDocument owner;
        fz_context* context = owner.context();
        fz_document* document = nullptr;
        fz_stext_page* text = nullptr;
        fz_var(document);
        fz_var(text);
        fz_rect imageBounds = fz_empty_rect;
        fz_rect firstImageBounds = fz_empty_rect;
        fz_rect beforeBounds = fz_empty_rect;
        fz_rect afterBounds = fz_empty_rect;
        float inlineBaseline = 0;
        int imageCount = 0;
        bool fitsPage = true;
        bool failed = false;
        fz_stext_options options { };
        options.flags = FZ_STEXT_PRESERVE_IMAGES;
        const QByteArray path = QFile::encodeName(html.fileName());
        fz_try(context)
        {
            document = fz_open_document(context, path.constData());
            fz_layout_document(context, document, 240, 240, 12);
            for (int page = 0; page < fz_count_pages(context, document); ++page) {
                text = fz_new_stext_page_from_page_number(context, document, page, &options);
                for (auto* block = text->first_block; block; block = block->next) {
                    if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                        ++imageCount;
                        fitsPage = fitsPage && block->bbox.y0 >= 12 - 0.01f && block->bbox.y1 <= 228 + 0.01f;
                        fz_rect bounds = block->bbox;
                        bounds.y0 += static_cast<float>(page) * 240;
                        bounds.y1 += static_cast<float>(page) * 240;
                        if (imageCount == 1)
                            firstImageBounds = bounds;
                        imageBounds = fz_union_rect(imageBounds, bounds);
                    } else if (block->type == FZ_STEXT_BLOCK_TEXT) {
                        for (auto* line = block->u.t.first_line; line; line = line->next) {
                            if (!line->first_char)
                                continue;
                            fz_rect bounds = line->bbox;
                            bounds.y0 += static_cast<float>(page) * 240;
                            bounds.y1 += static_cast<float>(page) * 240;
                            if (line->first_char->c == 'B')
                                beforeBounds = bounds;
                            else if (line->first_char->c == 'A')
                                afterBounds = bounds;
                            else if (line->first_char->c == 'I')
                                inlineBaseline = line->first_char->origin.y + static_cast<float>(page) * 240;
                        }
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
            failed = true;
        }
        QVERIFY2(!failed, fz_caught_message(context));
        QCOMPARE(imageCount, secondImage ? 2 : 1);
        QVERIFY2(fitsPage,
                 qPrintable(QStringLiteral("Image page bounds: %1, %2").arg(imageBounds.y0).arg(imageBounds.y1)));
        QVERIFY(!fz_is_empty_rect(beforeBounds));
        QVERIFY(!fz_is_empty_rect(afterBounds));
        QVERIFY2(beforeBounds.y1 <= imageBounds.y0 + 0.01f,
                 qPrintable(QStringLiteral("Preceding paragraph bottom %1 exceeds image top %2")
                                .arg(beforeBounds.y1)
                                .arg(imageBounds.y0)));
        QVERIFY2(
            afterBounds.y0 >= imageBounds.y1 - 0.01f,
            qPrintable(
                QStringLiteral("Caption top %1 precedes image bottom %2").arg(afterBounds.y0).arg(imageBounds.y1)));
        if (inlineText) {
            QVERIFY(inlineBaseline > 0);
            // Preserve author alignment rather than forcing every image to baseline.
            if (alignment == QStringLiteral("baseline"))
                QVERIFY(std::abs(firstImageBounds.y1 - inlineBaseline) < 0.01f);
            else if (alignment == QStringLiteral("middle"))
                QVERIFY(std::abs((firstImageBounds.y0 + firstImageBounds.y1) / 2 - (inlineBaseline - 3)) < 0.01f);
        }
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
        QVERIFY2(receiver.listen(socketPath, &error), error.c_str());
        ::Mu::IPC::FdChannel sender;
        QVERIFY(sender.connect(socketPath, &error));
        QVERIFY(receiver.accept(&error));

        ::Mu::Worker::Runtime::CommandService service({ .sandbox = { }, .fdChannel = &receiver });
        QVERIFY2(service.openFd(documentFd, "sample.epub", ::Mu::Model::DocumentType::Epub, &error), error.c_str());

        const int ocrFd = ::dup(file.handle());
        QVERIFY(ocrFd >= 0);
        QVERIFY(sender.send(7, ocrFd, &error));
        ::close(ocrFd);
        const auto response = service.dispatch({ 1, ::Mu::Model::OcrPageRequest { { 7 }, 0, 225, "eng" } });
        QVERIFY(response.error);
        QCOMPARE(response.error->code, ::Mu::Model::ErrorCode::Unavailable);
        QVERIFY(response.error->message.find("PDF") != std::string::npos);
    }

    void testRenderCancellationCookieValidation_data()
    {
        QTest::addColumn<int>("size");
        QTest::addColumn<int>("seals");
        QTest::addColumn<bool>("abort");
        QTest::addColumn<bool>("cancelled");
        constexpr int required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
        QTest::newRow("cancelled") << 64 << required << true << true;
        QTest::newRow("wrong-size") << 63 << required << true << false;
        QTest::newRow("unsealed") << 64 << 0 << true << false;
        QTest::newRow("mutable-seals") << 64 << (F_SEAL_SHRINK | F_SEAL_GROW) << true << false;
        QTest::newRow("write-sealed") << 64 << (required | F_SEAL_WRITE) << false << false;
    }

    void testRenderCancellationCookieValidation()
    {
        QFETCH(int, size);
        QFETCH(int, seals);
        QFETCH(bool, abort);
        QFETCH(bool, cancelled);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        std::string error;
        ::Mu::IPC::FdChannel receiver;
        QVERIFY(receiver.listen(directory.filePath(QStringLiteral("cancel.sock")).toStdString(), &error));
        ::Mu::IPC::FdChannel sender;
        QVERIFY(sender.connect(directory.filePath(QStringLiteral("cancel.sock")).toStdString(), &error));
        QVERIFY(receiver.accept(&error));
        ::Mu::Worker::Runtime::CommandService service({ .sandbox = { }, .fdChannel = &receiver });
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(service.openFd(::dup(file.handle()), "sample.epub", ::Mu::Model::DocumentType::Epub, &error));
        ::Mu::Worker::Sys::FileDescriptor cookie(::memfd_create("test-cookie", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        QVERIFY(cookie);
        QCOMPARE(::ftruncate(cookie.get(), size), 0);
        const std::int32_t flag = abort ? 1 : 0;
        QCOMPARE(::pwrite(cookie.get(), &flag, sizeof(flag), 0), ssize_t(sizeof(flag)));
        QCOMPARE(::fcntl(cookie.get(), F_ADD_SEALS, seals), 0);
        QVERIFY(sender.send(7, cookie.get(), &error));
        const auto response = service.dispatch({ 100, ::Mu::Model::RenderRequest { 0, 100, 100, std::nullopt, 7 } });
        QCOMPARE(response.id, std::uint64_t(100));
        QVERIFY(response.error);
        QCOMPARE(response.error->code,
                 cancelled ? ::Mu::Model::ErrorCode::Cancelled : ::Mu::Model::ErrorCode::InvalidRequest);
        QVERIFY(std::holds_alternative<std::monostate>(response.payload));
        pollfd pending { sender.fd(), POLLIN, 0 };
        QCOMPARE(::poll(&pending, 1, 0), 0); // No partial frame descriptor.
        const auto next = service.dispatch({ 101, ::Mu::Model::RenderRequest { 0, 100, 100, std::nullopt } });
        QVERIFY(!next.error);
        const auto* rendered = std::get_if<::Mu::Model::RenderResponse>(&next.payload);
        QVERIFY(rendered);
        ::Mu::Worker::Sys::FileDescriptor frame(sender.receive(rendered->frame.transferId, &error));
        QVERIFY(frame);
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

    void testExportPdfRendersPagesAndLinks_data()
    {
        QTest::addColumn<bool>("withReferences");
        QTest::newRow("print") << false;
        QTest::newRow("export") << true;
    }

    void testExportPdfRendersPagesAndLinks()
    {
        QFETCH(bool, withReferences);
        QFile file(QStringLiteral(TEST_EPUB_DIR "/linked.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "linked.epub", &error), error.c_str());
        file.close();
        QVERIFY(document.pageCount() > 1);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString fullPath = directory.filePath(QStringLiteral("exported.pdf"));
        const int fullFd = ::open(fullPath.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fullFd >= 0);
        const bool saved = withReferences ? document.savePdfFdWithReferences(fullFd, { }, &error)
                                          : document.savePdfFd(fullFd, { }, &error);
        QVERIFY2(saved, error.c_str());

        QFile fullFile(fullPath);
        QVERIFY(fullFile.open(QIODevice::ReadOnly));
        QVERIFY(fullFile.read(5) == "%PDF-");
        ::Mu::Worker::Engine::PdfDocument fullPdf;
        QVERIFY2(fullPdf.openFd(::dup(fullFile.handle()), "exported.pdf", &error), error.c_str());
        fullFile.close();
        QCOMPARE(fullPdf.pageCount(), document.pageCount());

        // The EPUB Title/Author and the exporter's Producer reach the PDF Info
        // dictionary through the references export path.
        const auto exportedMeta = fullPdf.metadata({ "title", "author", "producer" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(QString::fromStdString(exportedMeta.values.at("title")), QStringLiteral("Linked Test EPUB"));
        QCOMPARE(QString::fromStdString(exportedMeta.values.at("author")), QStringLiteral("Linked Author"));
        QCOMPARE(QString::fromStdString(exportedMeta.values.at("producer")),
                 QStringLiteral("Okular/okular-mupdf-ng ") + QString::fromStdString(std::string(::Mu::IPC::COMPAT)));

        // Page 0 must carry the forward internal reference (the regression
        // class of "cannot find page N in page tree") and the external URI.
        const auto links = fullPdf.extractLinks(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        bool forwardInternal = false;
        bool external = false;
        for (const auto& link : links) {
            if (link.target.valid && !link.target.external && link.target.viewport.page > 0)
                forwardInternal = true;
            if (link.target.external && link.target.uri == "https://example.org/external")
                external = true;
        }
        QCOMPARE(forwardInternal, withReferences);
        QCOMPARE(external, withReferences);
        QCOMPARE(!fullPdf.outline(&error).empty(), withReferences);
        if (!withReferences) {
            for (int page = 0; page < fullPdf.pageCount(); ++page)
                QVERIFY(fullPdf.extractLinks(page, &error).empty());
        }
    }

    void testExportPdfOutlineNesting_data()
    {
        QTest::addColumn<bool>("subset");
        QTest::addColumn<bool>("reverse");
        QTest::newRow("full") << false << false;
        QTest::newRow("subset") << true << false;
        QTest::newRow("reversed") << false << true;
    }

    void testExportPdfOutlineNesting()
    {
        QFETCH(bool, subset);
        QFETCH(bool, reverse);
        QFile file(QStringLiteral(TEST_EPUB_DIR "/linked.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "linked.epub", &error), error.c_str());
        file.close();

        const auto sourceNodes = document.outline(&error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(sourceNodes.size(), std::size_t(2));
        QCOMPARE(sourceNodes.front().children.size(), std::size_t(1));
        std::vector<int> pages;
        for (int page = 0; page < document.pageCount(); ++page)
            pages.push_back(page);
        if (subset)
            pages.resize(1);
        if (reverse)
            std::reverse(pages.begin(), pages.end());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString fullPath = directory.filePath(QStringLiteral("exported.pdf"));
        const int fullFd = ::open(fullPath.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fullFd >= 0);
        QVERIFY2(document.savePdfFdWithReferences(fullFd, pages, &error), error.c_str());

        QFile fullFile(fullPath);
        QVERIFY(fullFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument fullPdf;
        QVERIFY2(fullPdf.openFd(::dup(fullFile.handle()), "exported.pdf", &error), error.c_str());
        fullFile.close();

        // The nesting regression placed "Chapter Two" inside "Chapter One";
        // both must stay top level with the nested section under the first.
        const auto nodes = fullPdf.outline(&error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(nodes.size(), std::size_t(2));
        QCOMPARE(QString::fromStdString(nodes.front().title), QStringLiteral("Chapter One"));
        QCOMPARE(nodes.front().children.size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(nodes.front().children.front().title), QStringLiteral("Section 1.1"));
        QCOMPARE(QString::fromStdString(nodes.back().title), QStringLiteral("Chapter Two"));

        const auto verifyDestination = [&](const auto& exported, const auto& source) {
            QVERIFY(source.link.valid);
            QVERIFY(!source.link.external);
            const auto target = std::find(pages.begin(), pages.end(), source.link.viewport.page);
            QCOMPARE(exported.link.valid, target != pages.end());
            if (target != pages.end()) {
                QVERIFY(!exported.link.external);
                QCOMPARE(exported.link.viewport.page, static_cast<int>(std::distance(pages.begin(), target)));
            }
        };
        verifyDestination(nodes.front(), sourceNodes.front());
        verifyDestination(nodes.front().children.front(), sourceNodes.front().children.front());
        verifyDestination(nodes.back(), sourceNodes.back());
    }

    void testExportPdfSubsetAndErrors()
    {
        QFile file(QStringLiteral(TEST_EPUB_DIR "/linked.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::EpubDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "linked.epub", &error), error.c_str());
        file.close();

        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        // A page subset must succeed even though page 0 links forward to an
        // excluded destination page; the missing target is skipped while the
        // external URI survives.
        const QString subsetPath = directory.filePath(QStringLiteral("subset.pdf"));
        const int subsetFd = ::open(subsetPath.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        QVERIFY(subsetFd >= 0);
        QVERIFY2(document.savePdfFdWithReferences(subsetFd, { 0 }, &error), error.c_str());

        QFile subsetFile(subsetPath);
        QVERIFY(subsetFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument subsetPdf;
        QVERIFY2(subsetPdf.openFd(::dup(subsetFile.handle()), "subset.pdf", &error), error.c_str());
        subsetFile.close();
        QCOMPARE(subsetPdf.pageCount(), 1);
        const auto links = subsetPdf.extractLinks(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        const bool external = std::any_of(links.cbegin(), links.cend(), [](const auto& link) {
            return link.target.external && link.target.uri == "https://example.org/external";
        });
        QVERIFY(external);

        // Invalid descriptors and out-of-range selections must fail cleanly.
        error.clear();
        QVERIFY(!document.savePdfFdWithReferences(-1, { }, &error));
        QVERIFY(!error.empty());

        const QString rejectPath = directory.filePath(QStringLiteral("rejected.pdf"));
        const int rejectFd = ::open(rejectPath.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        QVERIFY(rejectFd >= 0);
        QVERIFY(!document.savePdfFdWithReferences(rejectFd, { document.pageCount() }, &error));
        QVERIFY(!error.empty());
    }

    void testExportJobsRejectsUnsupportedTypes_data()
    {
        QTest::addColumn<int>("type");
        QTest::newRow("pdf") << int(::Mu::Model::DocumentType::Pdf);
        QTest::newRow("unknown") << int(::Mu::Model::DocumentType::Unknown);
    }

    void testExportJobsRejectsUnsupportedTypes()
    {
        QFETCH(int, type);
        ::Mu::Worker::Engine::ExportJobs jobs;
        QVERIFY(jobs.eventFd() >= 0);
        const int inFd = ::open("/dev/null", O_RDONLY);
        const int outFd = ::open("/dev/null", O_WRONLY);
        QVERIFY(inFd >= 0);
        QVERIFY(outFd >= 0);
        QVERIFY(!jobs.submit(inFd, outFd, { }, { }, static_cast<::Mu::Model::DocumentType>(type)));
        QVERIFY(::fcntl(inFd, F_GETFD) == -1 && errno == EBADF);
        QVERIFY(::fcntl(outFd, F_GETFD) == -1 && errno == EBADF);
    }

    void testExportJobsRejectsSubmissionsWithoutCompletionChannel()
    {
        // A dead completion eventfd strands every job silently, so the runner
        // must reject submissions and close the offered descriptors.
        ::Mu::Worker::Sys::FileDescriptor dead;
        ::Mu::Worker::Engine::ExportJobs jobs(std::move(dead));
        QCOMPARE(jobs.eventFd(), -1);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        int outFd = ::open(
            directory.filePath(QStringLiteral("rejected.pdf")).toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outFd >= 0);
        int inFd = ::dup(file.handle());
        QVERIFY(inFd >= 0);
        QVERIFY(!jobs.submit(inFd, outFd, { }, { }).has_value());
        // Rejection consumed both descriptors.
        const bool inClosed = ::fcntl(inFd, F_GETFD) == -1 && errno == EBADF;
        QVERIFY(inClosed);
        const bool outClosed = ::fcntl(outFd, F_GETFD) == -1 && errno == EBADF;
        QVERIFY(outClosed);
        file.close();
    }

    void testExportPdfAsyncRejectionClosesDescriptors()
    {
        // Every rejection path in exportPdfAsyncResponse must consume both
        // offered descriptors; the eventfd-rejection branch previously leaked
        // them when a dead completion channel was reported.
        ::Mu::Worker::Runtime::CommandService service({ });

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const int outFd = ::open(
            directory.filePath(QStringLiteral("rejected.pdf")).toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outFd >= 0);
        const int inFd = ::open("/dev/null", O_RDONLY);
        QVERIFY(inFd >= 0);

        const auto response =
            service.exportPdfAsyncResponse({ 1, ::Mu::Model::ExportPdfAsyncRequest { { 201 }, { 202 }, { } } },
                                           ::Mu::Model::ExportPdfAsyncRequest { { 201 }, { 202 }, { } },
                                           outFd,
                                           inFd);
        QVERIFY2(response.error, "export without a document must be rejected");

        QVERIFY(::fcntl(outFd, F_GETFD) == -1 && errno == EBADF);
        QVERIFY(::fcntl(inFd, F_GETFD) == -1 && errno == EBADF);
    }

    void testExportJobsBackgroundCompletion()
    {
        ::Mu::Worker::Engine::ExportJobs jobs;
        QVERIFY(jobs.eventFd() >= 0);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const ::Mu::Model::DocumentSettings settings;

        const QString outputPath = directory.filePath(QStringLiteral("job.pdf"));
        int outFd = ::open(outputPath.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outFd >= 0);
        const auto jobId = jobs.submit(::dup(file.handle()), outFd, settings, { });
        QVERIFY(jobId.has_value());

        const auto completed = waitForExportNotifications(jobs, 1);
        QCOMPARE(completed.size(), std::size_t(1));
        QCOMPARE(completed.front().id, *jobId);
        QVERIFY(completed.front().success);
        QVERIFY(completed.front().error.empty());
        verifyPdfFile(outputPath);

        // A finished job frees the single in-flight slot for the next export.
        const QString jobPath = directory.filePath(QStringLiteral("job2.pdf"));
        outFd = ::open(jobPath.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outFd >= 0);
        const auto secondJob = jobs.submit(::dup(file.handle()), outFd, settings, { });
        QVERIFY(secondJob.has_value());
        file.close();

        const auto secondCompleted = waitForExportNotifications(jobs, 1);
        QCOMPARE(secondCompleted.size(), std::size_t(1));
        QCOMPARE(secondCompleted.front().id, *secondJob);
        QVERIFY(secondCompleted.front().success);
        QVERIFY(secondCompleted.front().error.empty());
        verifyPdfFile(jobPath);
    }

    void testAsyncPdfExportDispatch()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(QStringLiteral(TEST_EPUB_DIR "/sample.epub"));
        QVERIFY(file.open(QIODevice::ReadOnly));

        // A channel-less service rejects the transfer lookup instantly; the
        // fd-backed service below exercises the full happy path.
        std::string error;
        ::Mu::Worker::Runtime::CommandService plainService({ });
        QVERIFY2(plainService.openFd(::dup(file.handle()), "sample.epub", ::Mu::Model::DocumentType::Epub, &error),
                 error.c_str());
        const auto noChannel =
            plainService.dispatch({ 1, ::Mu::Model::ExportPdfAsyncRequest { { 201 }, { 202 }, { } } });
        QVERIFY(noChannel.error);
        QCOMPARE(noChannel.error->code, ::Mu::Model::ErrorCode::InvalidRequest);

        ::Mu::IPC::FdChannel receiver;
        const auto socketPath = directory.filePath(QStringLiteral("fd.sock")).toStdString();
        QVERIFY2(receiver.listen(socketPath, &error), error.c_str());
        ::Mu::IPC::FdChannel sender;
        QVERIFY(sender.connect(socketPath, &error));
        QVERIFY(receiver.accept(&error));

        ::Mu::Worker::Runtime::CommandService service({ .sandbox = { }, .fdChannel = &receiver });
        QVERIFY2(service.openFd(::dup(file.handle()), "sample.epub", ::Mu::Model::DocumentType::Epub, &error),
                 error.c_str());

        const QString outputPath = directory.filePath(QStringLiteral("dispatched.pdf"));
        int outFd = ::open(outputPath.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outFd >= 0);
        QVERIFY(sender.send(201, outFd, &error));
        ::close(outFd);
        int inFd = ::dup(file.handle());
        QVERIFY(inFd >= 0);
        QVERIFY(sender.send(202, inFd, &error));
        ::close(inFd);
        const auto response = service.dispatch({ 2, ::Mu::Model::ExportPdfAsyncRequest { { 201 }, { 202 }, { } } });
        QVERIFY2(!response.error, response.error ? response.error->message.c_str() : "");
        const auto* job = std::get_if<::Mu::Model::JobResponse>(&response.payload);
        QVERIFY(job);
        QVERIFY(job->jobId != 0);

        const auto notifications = waitForExportNotifications(service, 1);
        QCOMPARE(notifications.size(), std::size_t(1));
        QCOMPARE(notifications.front().id, job->jobId);
        QVERIFY(notifications.front().success);
        QVERIFY(notifications.front().error.empty());
        verifyPdfFile(outputPath);

        // The session document stays fully usable while and after the job ran.
        const auto text = service.dispatch({ 3, ::Mu::Model::TextBoxesRequest { 0, 72, 72, true } });
        QVERIFY2(!text.error, text.error ? text.error->message.c_str() : "");
    }
};

int runTestWorkerEpub(int argc, char** argv)
{
    TestEpub test;
    return QTest::qExec(&test, argc, argv);
}

#include "epub.moc"
