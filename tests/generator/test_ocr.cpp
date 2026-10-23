// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KActionCollection>
#include <KConfigDialog>
#include <KConfigGroup>
#include <KConfigSkeleton>
#include <KSharedConfig>
#include <KXMLGUIBuilder>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <QAction>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMimeDatabase>
#include <QPainter>
#include <QPdfWriter>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <okular/core/action.h>
#include <okular/core/document.h>
#include <okular/core/generator.h>
#include <okular/core/observer.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>
#include <okular/core/tile.h>
#include <okular/interfaces/configinterface.h>

#include <algorithm>
#include <memory>

#include "generator/config/settings.hpp"
#include "generator/okular_compat.hpp"
#if !MU_OKULAR_PAGE_PLACEHOLDER_COLOR
#include "generator/page/placeholder.hpp"
#endif

class TestGuiClient : public KXMLGUIClient {
public:
    TestGuiClient()
    {
        actionCollection()->addAction(QStringLiteral("previous"),
                                      new QAction(QStringLiteral("Previous"), actionCollection()));
        setXML(QStringLiteral("<gui name=\"test\" translationDomain=\"okular_mupdfng\"><MenuBar><Menu name=\"tools\">"
                              "<text>Tools</text><Action name=\"previous\"/></Menu></MenuBar></gui>"));
    }
};

class TestMenuClient : public KXMLGUIClient {
public:
    TestMenuClient(const QStringList& menus, bool shell = false)
    {
        QString xml = QStringLiteral("<gui name=\"%1\" translationDomain=\"okular_mupdfng\"><MenuBar>")
                          .arg(shell ? QStringLiteral("test_shell") : QStringLiteral("test_part"));
        for (const auto& name : menus) {
            if (shell && name == QStringLiteral("settings"))
                xml += QStringLiteral("<Merge/>");
            actionCollection()->addAction(name, new QAction(name, actionCollection()));
            xml += QStringLiteral("<Menu name=\"%1\"><text>%1</text><Action name=\"%1\"/></Menu>").arg(name);
        }
        setXML(xml + QStringLiteral("</MenuBar></gui>"));
    }
};

class TestGeneratorOcr : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;

    static bool writePdfObjects(const QString& path, const QList<QByteArray>& objects)
    {
        QByteArray pdf("%PDF-1.4\n");
        QList<qsizetype> offsets { 0 };
        for (qsizetype i = 0; i < objects.size(); ++i) {
            offsets.append(pdf.size());
            pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const qsizetype xref = pdf.size();
        const QByteArray count = QByteArray::number(objects.size() + 1);
        pdf += "xref\n0 " + count + "\n0000000000 65535 f \n";
        for (qsizetype i = 1; i < offsets.size(); ++i)
            pdf += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
        pdf += "trailer\n<< /Size " + count + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(pdf) == pdf.size();
    }

    static bool writePreviewPdf(const QString& path)
    {
        // A minimal valid PDF with an internal link, without depending on text
        // extraction or generated outlines to supply an action.
        const QList<QByteArray> objects {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 900] /Resources << >> /Annots [4 0 R] >>",
            "<< /Type /Annot /Subtype /Link /Rect [10 10 100 100] /Dest [3 0 R /XYZ 0 450 null] >>"
        };
        return writePdfObjects(path, objects);
    }

    static QWidget* previewWindow()
    {
        for (auto* window : QApplication::topLevelWidgets())
            if (window->objectName() == QStringLiteral("mupdfLinkPreview") && window->isVisible())
                return window;
        return nullptr;
    }

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", m_root.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", m_root.filePath("cache").toUtf8());
        QCoreApplication::setLibraryPaths({ QStringLiteral(TEST_PLUGIN_ROOT) });
        qputenv("PATH", QByteArray(TEST_WORKER_DIR) + ':' + qgetenv("PATH"));
        Okular::SettingsCore::instance(QStringLiteral("mupdfng-ocr-test"));
        // Keep asynchronous cache-maintenance settings writes out of OCR configuration tests.
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup advanced(config, QStringLiteral("Advanced"));
        advanced.writeEntry("CacheLastVacuum", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        config->sync();
    }

    void ocrMenuAvailability_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("pdf") << QStringLiteral(TEST_PDF_PATH) << true;
        QTest::newRow("epub") << QStringLiteral(TEST_EPUB_PATH) << false;
        QTest::newRow("mobi") << QStringLiteral(TEST_MOBI_PATH) << false;
    }

    void ocrMenuAvailability()
    {
        QFETCH(QString, path);
        QFETCH(bool, enabled);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        config->sync();
        Okular::Document document(nullptr);
        // Reopen a PDF after each format to check that the cached generator resets the menu state.
        for (const auto& currentPath : { path, QStringLiteral(TEST_PDF_PATH) }) {
            QCOMPARE(document.openDocument(
                         currentPath, QUrl::fromLocalFile(currentPath), QMimeDatabase().mimeTypeForFile(currentPath)),
                     Okular::Document::OpenSuccess);
            auto* client = document.guiClient();
            QVERIFY(client);
            auto* action = client->actionCollection()->action(QStringLiteral("mupdf_ocr"));
            QVERIFY(action);
            const bool expected = currentPath == QStringLiteral(TEST_PDF_PATH) || enabled;
            const bool hasModels =
                !Mu::Generator::Config::installedOcrModels({ QStringLiteral(TESSDATA_DIR) }).isEmpty();
            QCOMPARE(action->isEnabled(), expected);
            for (QAction* registered : client->actionCollection()->actions())
                QCOMPARE(registered->isEnabled(),
                         registered->objectName() == QStringLiteral("mupdf_manage_certificates")
                             || registered->objectName().startsWith(QStringLiteral("mupdf_theme"))
                             || (expected && (registered == action || hasModels)));
            document.reparseConfig();
            QCOMPARE(action->isEnabled(), expected);
            document.closeDocument();
            QVERIFY(!action->isEnabled());
        }
    }

#if !MU_OKULAR_PAGE_PLACEHOLDER_COLOR
    void loadingPixmapsRemainIncomplete_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<bool>("tiled");
        QTest::addColumn<bool>("rotated");
        QTest::addColumn<bool>("asynchronous");
        QTest::addColumn<bool>("themed");
        QTest::newRow("page") << 300 << 400 << false << false << true << true;
        QTest::newRow("zoom") << 900 << 1200 << false << false << true << true;
        QTest::newRow("tiles") << 6000 << 8000 << true << false << true << true;
        QTest::newRow("rotated") << 400 << 300 << false << true << true << true;
        QTest::newRow("rotated-tiles") << 8000 << 6000 << true << true << true << true;
        QTest::newRow("synchronous") << 300 << 400 << false << false << false << true;
        QTest::newRow("default") << 300 << 400 << false << false << true << false;
        QTest::newRow("default-rotated") << 400 << 300 << false << true << true << false;
        QTest::newRow("default-tiles") << 6000 << 8000 << true << false << true << false;
        QTest::newRow("default-synchronous") << 300 << 400 << false << false << false << false;
    }

    void loadingPixmapsRemainIncomplete()
    {
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(bool, tiled);
        QFETCH(bool, rotated);
        QFETCH(bool, asynchronous);
        QFETCH(bool, themed);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("ReadingTheme", themed ? "Dark" : "Default");
        config->sync();
        const auto restore = qScopeGuard([&] {
            general.writeEntry("ReadingTheme", "Default");
            config->sync();
        });
        Okular::Document document(nullptr);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        if (rotated)
            document.setRotation(Okular::Rotation90);
        const Okular::NormalizedRect region =
            tiled ? Okular::NormalizedRect(0, 0, 0.05, 0.05) : Okular::NormalizedRect(0, 0, 1, 1);

        struct Observer : Okular::DocumentObserver {
            const Okular::Page* page = nullptr;
            int width = 0;
            int height = 0;
            Okular::NormalizedRect region;
            bool sawIncomplete = false;
            bool sawLoadingPixmap = false;
            int completed = 0;
            bool sawColoredTiles = false;
            bool invalidPreviewPixels = false;

            void notifyPageChanged(int pageNumber, int flags) override
            {
                if (pageNumber != page->number() || !(flags & Pixmap))
                    return;
                if (page->hasPixmap(this, width, height, region))
                    ++completed;
                else {
                    sawIncomplete = true;
                    sawLoadingPixmap |= page->hasPixmap(this);
                    if (completed == 0 && page->hasTilesManager(this)) {
                        for (const auto& tile : page->tilesAt(this, { 0, 0, 1, 1 })) {
                            if (!tile.pixmap() || tile.pixmap()->isNull())
                                continue;
                            sawColoredTiles = true;
                            const QImage image = tile.pixmap()->toImage();
                            invalidPreviewPixels |= image.pixelColor(0, 0) != QColor(32, 33, 36)
                                || image.pixelColor(image.width() - 1, image.height() - 1) != QColor(32, 33, 36);
                        }
                    }
                }
            }
        } observer;

        observer.page = document.page(0);
        observer.width = width;
        observer.height = height;
        observer.region = region;
        document.addObserver(&observer);
        const auto remove = qScopeGuard([&] { document.removeObserver(&observer); });
        document.setVisiblePageRects({ new Okular::VisiblePageRect(0, region) });
        auto request = [&] {
            auto* pixmap = new Okular::PixmapRequest(&observer,
                                                     0,
                                                     width,
                                                     height,
                                                     1,
                                                     0,
                                                     asynchronous ? Okular::PixmapRequest::Asynchronous
                                                                  : Okular::PixmapRequest::NoFeature);
            if (tiled)
                pixmap->setNormalizedRect(region);
            document.requestPixmaps({ pixmap });
        };
        // Exercise initial loading, an explicit refresh (which also sees the
        // unregistered preview observer), and a second render after invalidation.
        for (int attempt = 0; attempt < 2; ++attempt) {
            observer.sawIncomplete = false;
            observer.sawLoadingPixmap = false;
            request();
            QTRY_VERIFY_WITH_TIMEOUT(observer.page->hasPixmap(&observer, width, height, region), 10000);
            if (asynchronous && themed)
                QVERIFY(observer.sawIncomplete);
            if (!themed)
                QVERIFY(!observer.sawLoadingPixmap);
            QCOMPARE(observer.page->hasTilesManager(&observer), tiled);
            if (tiled && !rotated && themed)
                QVERIFY(observer.sawColoredTiles);
            if (!themed)
                QVERIFY(!observer.sawColoredTiles);
            QVERIFY(!observer.invalidPreviewPixels);
            if (attempt == 0) {
                const int previousCompletions = observer.completed;
                document.refreshPixmaps(0);
                QTRY_VERIFY_WITH_TIMEOUT(observer.completed > previousCompletions, 10000);
                const_cast<Okular::Page*>(observer.page)->deletePixmaps();
            }
        }
    }

    void placeholderRotationLifecycle_data()
    {
        QTest::addColumn<int>("rotation");
        QTest::newRow("clockwise") << int(Okular::Rotation90);
        QTest::newRow("counterclockwise") << int(Okular::Rotation270);
    }

    void placeholderRotationLifecycle()
    {
        QFETCH(int, rotation);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("ReadingTheme", "Default");
        config->sync();
        Okular::Document document(nullptr);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        // Okular restores the last rotation for this fixture between test rows.
        document.setRotation(Okular::Rotation0);
        Mu::Generator::Page::Placeholder placeholder;
        auto* page = const_cast<Okular::Page*>(document.page(0));
        placeholder.setTheme(Mu::Model::DocumentTheme::Dark);
        placeholder.seed(*page);

        struct Observer : Okular::DocumentObserver {
            Mu::Generator::Page::Placeholder* placeholder = nullptr;
            Okular::Page* page = nullptr;
            bool sawVertical = false;
            bool restoredHorizontal = false;
            bool clearing = false;
            bool removedLatePreview = false;

            void notifyPageChanged(int number, int flags) override
            {
                if (number != 0 || !(flags & Pixmap))
                    return;
                sawVertical |= page->hasPixmap(placeholder, 1, 256);
                restoredHorizontal |= sawVertical && page->hasPixmap(placeholder, 256, 1);
                const bool hadPreview = page->hasPixmap(placeholder);
                placeholder->refresh(*page);
                removedLatePreview |= clearing && hadPreview && !page->hasPixmap(placeholder);
            }
        } observer;

        observer.placeholder = &placeholder;
        observer.page = page;
        document.addObserver(&observer);
        const auto cleanup = qScopeGuard([&] {
            document.removeObserver(&observer);
            document.closeDocument();
        });
        document.setRotation(static_cast<Okular::Rotation>(rotation));
        QTRY_VERIFY(observer.restoredHorizontal);
        QVERIFY(page->hasPixmap(&placeholder, 256, 1));
        // The queued Dusk rotation must not resurrect a preview after reset.
        placeholder.setTheme(Mu::Model::DocumentTheme::Dusk);
        placeholder.seed(*page);
        placeholder.setTheme(Mu::Model::DocumentTheme::Default);
        placeholder.seed(*page);
        observer.clearing = true;
        QTRY_VERIFY(observer.removedLatePreview);
        QVERIFY(!page->hasPixmap(&placeholder));
        // Close with another preview rotation still queued, exercising teardown.
        placeholder.setTheme(Mu::Model::DocumentTheme::Dark);
        placeholder.seed(*page);
    }

    void placeholderRenderLifecycle_data()
    {
        QTest::addColumn<bool>("closeImmediately");
        QTest::newRow("cancel-and-replace") << false;
        QTest::newRow("close-during-render") << true;
    }

    void placeholderRenderLifecycle()
    {
        QFETCH(bool, closeImmediately);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("ReadingTheme", "Dark");
        config->sync();
        const auto restore = qScopeGuard([&] {
            general.writeEntry("ReadingTheme", "Default");
            config->sync();
        });
        Okular::Document document(nullptr);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();

        struct Observer : Okular::DocumentObserver {
            Okular::Document* document = nullptr;
            bool replace = false;
            bool requestedReplacement = false;

            void notifyPageChanged(int number, int flags) override
            {
                if (number != 0 || !(flags & Pixmap) || !replace || requestedReplacement
                    || document->page(0)->hasPixmap(this, 600, 850))
                    return;
                requestedReplacement = true;
                // A view may supersede a request inside the partial notification.
                document->requestPixmaps(
                    { new Okular::PixmapRequest(this, 0, 900, 1200, 1, 0, Okular::PixmapRequest::Asynchronous) });
            }
        } observer;

        observer.document = &document;
        observer.replace = !closeImmediately;
        document.addObserver(&observer);
        const auto cleanup = qScopeGuard([&] { document.removeObserver(&observer); });
        document.requestPixmaps(
            { new Okular::PixmapRequest(&observer, 0, 600, 850, 1, 0, Okular::PixmapRequest::Asynchronous) });
        if (!closeImmediately) {
            QVERIFY(observer.requestedReplacement);
            QTRY_VERIFY_WITH_TIMEOUT(document.page(0)->hasPixmap(&observer, 900, 1200), 10000);
            QVERIFY(!document.page(0)->hasPixmap(&observer, 600, 850));
        }
        document.closeDocument();
    }

#endif

    void linkPreviewSettingsApplyToOpenDocuments()
    {
        const QString path = m_root.filePath(QStringLiteral("preview-link.pdf"));
        QVERIFY(writePreviewPdf(path));
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("LinkPreviewMode", "Off");
        config->sync();

        QWidget host;

        host.setGeometry(20, 20, 760, 560);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        Okular::Document first(nullptr), second(nullptr);
        for (auto* document : { &first, &second }) {
            QCOMPARE(document->openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                     Okular::Document::OpenSuccess);
            document->reparseConfig();
            QTRY_VERIFY(!document->page(0)->objectRects().isEmpty());
        }
        const auto* theme = first.guiClient()->actionCollection()->action(QStringLiteral("mupdf_theme"));
        QVERIFY(theme);
        auto* settings = qobject_cast<Okular::ConfigInterface*>(theme->parent());
        QVERIFY(settings);
        const QStringList labels { "Off", "Adaptive Size", "Small", "Large" };
        const QStringList values { "Off", "AutoSize", "Small", "Large" };
        for (int mode : { 1, 2, 3, 0 }) {
            {
                KConfigSkeleton hostSettings;
                KConfigDialog dialog(nullptr, QStringLiteral("link-preview-test"), &hostSettings);
                settings->addPages(&dialog);
                auto* combo = dialog.findChild<QComboBox*>(QStringLiteral("kcfg_LinkPreviewMode"));
                QVERIFY(combo);
                QCOMPARE(combo->count(), labels.size());
                for (int i = 0; i < labels.size(); ++i)
                    QCOMPARE(combo->itemText(i), labels[i]);
                auto* buttons = dialog.findChild<QDialogButtonBox*>();
                QVERIFY(buttons);
                auto* ok = buttons->button(QDialogButtonBox::Ok);
                QVERIFY(ok);
                dialog.show();
                combo->setCurrentIndex(mode);
                ok->click();
            }
            // Only the first document receives Okular's explicit reparse call.
            // The shared preference must also update the second open document.
            first.reparseConfig();
            QCOMPARE(general.readEntry("LinkPreviewMode", QStringLiteral("Off")), values[mode]);
            for (auto* document : { &first, &second }) {
                QTest::mouseMove(&host, QPoint(40, 40));
                QTRY_COMPARE(QApplication::widgetAt(QCursor::pos()), &host);
                const auto* link =
                    static_cast<const Okular::Action*>(document->page(0)->objectRects().front()->object());
                QCOMPARE(link->actionTip().isEmpty(), mode != 0);
                if (mode != 0) {
                    QTRY_VERIFY(previewWindow());
                    QTRY_VERIFY(
                        !previewWindow()->findChild<QLabel*>(QStringLiteral("previewImage"))->pixmap().isNull());
                    QTest::keyClick(&host, Qt::Key_Escape);
                }
                QVERIFY(!previewWindow());
            }
        }
    }

    void linkPreviewUsesViewRenderScale_data()
    {
        QTest::addColumn<bool>("tiled");
        QTest::addColumn<bool>("rotated");
        QTest::newRow("page") << false << false;
        QTest::newRow("tile") << true << false;
        QTest::newRow("rotated-page") << false << true;
        QTest::newRow("rotated-tile") << true << true;
    }

    void linkPreviewUsesViewRenderScale()
    {
        QFETCH(bool, tiled);
        QFETCH(bool, rotated);
        const QString path = m_root.filePath(QStringLiteral("preview-scale.pdf"));
        QVERIFY(writePreviewPdf(path));
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("LinkPreviewMode", "Off");
        config->sync();
        const auto restore = qScopeGuard([&] {
            general.writeEntry("LinkPreviewMode", "Off");
            config->sync();
        });

        struct View final : QWidget, Okular::DocumentObserver {
        } host, thumbnail;

        host.setGeometry(20, 20, 760, 560);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        QTRY_VERIFY(!document.page(0)->objectRects().isEmpty());
        document.addObserver(&host);
        document.addObserver(&thumbnail);
        const auto removeObservers = qScopeGuard([&] {
            document.removeObserver(&host);
            document.removeObserver(&thumbnail);
        });
        if (rotated)
            document.setRotation(Okular::Rotation90);
        // Use full-page dimensions even for tiles; a later thumbnail must not
        // replace the scale belonging to the hovered view.
        for (auto* view : { &host, &thumbnail }) {
            const int width = view == &host ? (tiled ? 6000 : 300) : 60;
            const int height = view == &host ? (tiled ? 9000 : 450) : 90;
            const QSize size = rotated ? QSize(height, width) : QSize(width, height);
            const auto region = tiled && view == &host ? Okular::NormalizedRect(0.10, 0.10, 0.11, 0.11)
                                                       : Okular::NormalizedRect(0, 0, 1, 1);
            auto* request = new Okular::PixmapRequest(view,
                                                      0,
                                                      size.width(),
                                                      size.height(),
                                                      view->devicePixelRatioF(),
                                                      0,
                                                      Okular::PixmapRequest::Asynchronous);
            if (tiled && view == &host)
                request->setNormalizedRect(region);
            document.requestPixmaps({ request });
            const auto pixels = (QSizeF(size) * view->devicePixelRatioF()).toSize();
            QTRY_VERIFY_WITH_TIMEOUT(document.page(0)->hasPixmap(view, pixels.width(), pixels.height(), region), 10000);
            if (tiled && view == &host)
                QVERIFY(document.page(0)->hasTilesManager(view));
        }
        // Enabling previews must retain scales from pages rendered while Off.
        general.writeEntry("LinkPreviewMode", "AutoSize");
        config->sync();
        document.reparseConfig();
        QTest::mouseMove(&host, QPoint(40, 40));
        QTRY_COMPARE(QApplication::widgetAt(QCursor::pos()), &host);
        const auto* link = static_cast<const Okular::Action*>(document.page(0)->objectRects().front()->object());
        QVERIFY(link->actionTip().isEmpty());
        QTRY_VERIFY(previewWindow());
        auto* image = previewWindow()->findChild<QLabel*>(QStringLiteral("previewImage"));
        QVERIFY(image);
        const int width =
            tiled ? std::min(qRound(host.width() * 0.70), host.screen()->geometry().width() / 2 - 12) : 210;
        QTRY_COMPARE(image->pixmap().size(), QSize(width, qRound(width * 1.5)));
        QTest::keyClick(&host, Qt::Key_Escape);
        QVERIFY(!previewWindow());
    }

    void linkPreviewUsesCachedSourceScale_data()
    {
        QTest::addColumn<bool>("preload");
        QTest::newRow("rendered-source") << false;
        QTest::newRow("preloaded-source") << true;
    }

    void linkPreviewUsesCachedSourceScale()
    {
        QFETCH(bool, preload);
        const QString path = m_root.filePath(QStringLiteral("preview-cached-source.pdf"));
        QVERIFY(writePdfObjects(
            path,
            { "<< /Type /Catalog /Pages 2 0 R >>",
              "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
              "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 900] /Resources << >> /Annots [4 0 R] >>",
              "<< /Type /Annot /Subtype /Link /Rect [10 10 100 100] /Dest [5 0 R /XYZ 0 450 null] >>",
              "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 900] /Resources << >> >>" }));
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("LinkPreviewMode", "AutoSize");
        config->sync();
        const auto restore = qScopeGuard([&] {
            general.writeEntry("LinkPreviewMode", "Off");
            config->sync();
        });

        struct View final : QWidget, Okular::DocumentObserver {
        } view;

        view.setGeometry(20, 20, 760, 560);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        document.addObserver(&view);
        const auto remove = qScopeGuard([&] { document.removeObserver(&view); });
        for (int page : { 0, 1 }) {
            const int width = page == 0 ? 300 : 600;
            auto features = Okular::PixmapRequest::PixmapRequestFeatures(Okular::PixmapRequest::Asynchronous);
            if (page == 0 && preload)
                features |= Okular::PixmapRequest::Preload;
            document.requestPixmaps({ new Okular::PixmapRequest(
                &view, page, width, width * 3 / 2, view.devicePixelRatioF(), 0, features) });
            const auto pixels = (QSizeF(width, width * 1.5) * view.devicePixelRatioF()).toSize();
            QTRY_VERIFY_WITH_TIMEOUT(document.page(page)->hasPixmap(&view, pixels.width(), pixels.height()), 10000);
        }
        // Returning to page zero is a cache hit: the generator sees no new render.
        document.requestPixmaps({ new Okular::PixmapRequest(
            &view, 0, 300, 450, view.devicePixelRatioF(), 0, Okular::PixmapRequest::Asynchronous) });
        QTRY_VERIFY(!document.page(0)->objectRects().isEmpty());
        QTest::mouseMove(&view, QPoint(40, 40));
        QTRY_COMPARE(QApplication::widgetAt(QCursor::pos()), &view);
        const auto* link = static_cast<const Okular::Action*>(document.page(0)->objectRects().front()->object());
        QVERIFY(link->actionTip().isEmpty());
        QTRY_VERIFY(previewWindow());
        auto* image = previewWindow()->findChild<QLabel*>(QStringLiteral("previewImage"));
        QVERIFY(image);
        const int width = std::min(210, view.screen()->geometry().width() / 2 - 12);
        QTRY_COMPARE(image->pixmap().size(), QSize(width, qRound(width * 1.5)));
    }

    void documentThemeMenu_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("pdf") << QStringLiteral(TEST_PDF_PATH);
        QTest::newRow("epub") << QStringLiteral(TEST_EPUB_PATH);
        QTest::newRow("mobi") << QStringLiteral(TEST_MOBI_PATH);
    }

    void documentThemeMenu()
    {
        QFETCH(QString, path);
        const bool originalChangeColors = Okular::SettingsCore::changeColors();
        const int originalRenderMode = Okular::SettingsCore::renderMode();
        const QColor originalPaper = Okular::SettingsCore::paperColor();
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("ReadingTheme", "Default");
        config->sync();
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);

        struct Observer final : Okular::DocumentObserver {
            int refreshes = 0;

            void notifyContentsCleared(int flags) override
            {
                if (flags & Pixmap)
                    ++refreshes;
            }
        } observer;

        Okular::Document other(nullptr);
        QCOMPARE(other.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        other.addObserver(&observer);
        const auto cleanup = qScopeGuard([&] { other.removeObserver(&observer); });
        auto* actions = document.guiClient()->actionCollection();
        QVERIFY(actions->action(QStringLiteral("mupdf_theme"))->isEnabled());
        const auto menuItems = actions->action(QStringLiteral("mupdf_theme"))->menu()->actions();
        const QStringList menuOrder { "Default", "Twilight", "Dusk", "Dark" };
        QCOMPARE(menuItems.size(), menuOrder.size());
        for (int i = 0; i < menuOrder.size(); ++i)
            QCOMPARE(menuItems.at(i), actions->action(QStringLiteral("mupdf_theme_") + menuOrder.at(i).toLower()));
        const auto pageCount = document.pages();
        for (const auto* name :
             { "mupdf_theme_dusk", "mupdf_theme_dark", "mupdf_theme_twilight", "mupdf_theme_default" }) {
            auto* action = actions->action(QString::fromLatin1(name));
            QVERIFY(action && action->isEnabled() && action->isCheckable());
            const int previousRefreshes = observer.refreshes;
            action->trigger();
            QTRY_VERIFY(observer.refreshes > previousRefreshes);
            QVERIFY(action->isChecked());
            QCOMPARE(general.readEntry("ReadingTheme", QStringLiteral("Default")), action->text());
            QCOMPARE(document.pages(), pageCount);
            QVERIFY(document.isHistoryClean());
#if MU_OKULAR_PAGE_PLACEHOLDER_COLOR
            const QColor expectedColor = action->text() == QStringLiteral("Dark") ? QColor(32, 33, 36)
                : action->text() == QStringLiteral("Dusk")                        ? QColor(48, 49, 52)
                : action->text() == QStringLiteral("Twilight")                    ? QColor(208, 202, 184)
                                                                                  : QColor(Qt::white);
            for (int page = 0; page < static_cast<int>(document.pages()); ++page) {
                QCOMPARE(document.page(page)->placeholderColor(), expectedColor);
                QCOMPARE(other.page(page)->placeholderColor(), expectedColor);
            }
#endif
            QCOMPARE(Okular::SettingsCore::changeColors(), originalChangeColors);
            QCOMPARE(Okular::SettingsCore::renderMode(), originalRenderMode);
            QCOMPARE(Okular::SettingsCore::paperColor(), originalPaper);
        }
        const QStringList toggles { "dusk", "dusk", "dark", "dusk", "dark", "dark", "twilight", "twilight" };
        const QStringList expected { "Dusk", "Default", "Dark", "Dusk", "Dark", "Default", "Twilight", "Default" };
        for (int i = 0; i < toggles.size(); ++i) {
            auto* toggle = actions->action(QStringLiteral("mupdf_theme_toggle_") + toggles.at(i));
            QVERIFY(toggle && toggle->isEnabled());
            QVERIFY(KActionCollection::isShortcutsConfigurable(toggle));
            QVERIFY(toggle->shortcut().isEmpty());
            const int previousRefreshes = observer.refreshes;
            toggle->trigger();
            QTRY_VERIFY(observer.refreshes > previousRefreshes);
            QCOMPARE(general.readEntry("ReadingTheme", QStringLiteral("Default")), expected.at(i));
            QVERIFY(actions->action(QStringLiteral("mupdf_theme_") + expected.at(i).toLower())->isChecked());
            QVERIFY(document.isHistoryClean());
        }
        const QStringList themes {
            QStringLiteral("Default"), QStringLiteral("Twilight"), QStringLiteral("Dusk"), QStringLiteral("Dark")
        };
        // The theme menu is owned by the generator implementing ConfigInterface.
        auto* configInterface =
            qobject_cast<Okular::ConfigInterface*>(actions->action(QStringLiteral("mupdf_theme"))->parent());
        QVERIFY(configInterface);
        int previousMode = 0;
        for (const int mode : { 1, 2, 3, 0 }) {
            KConfigSkeleton hostSettings;
            KConfigDialog dialog(nullptr, QStringLiteral("document-theme-test"), &hostSettings);
            // Older Okular fillConfigDialog() requires its private BackendConfigDialog.
            // Exercise the plugin's public interface with our ordinary test dialog.
            configInterface->addPages(&dialog);
            auto* theme = dialog.findChild<QComboBox*>(QStringLiteral("kcfg_DocumentTheme"));
            QVERIFY(theme);
            QCOMPARE(theme->count(), 4);
            for (int i = 0; i < themes.size(); ++i)
                QCOMPARE(theme->itemText(i), themes.at(i));
            auto* buttons = dialog.findChild<QDialogButtonBox*>();
            QVERIFY(buttons);
            auto* ok = buttons->button(QDialogButtonBox::Ok);
            QVERIFY(ok);
            dialog.show();
            QCOMPARE(theme->currentIndex(), previousMode);
            const int previousRefreshes = observer.refreshes;
            theme->setCurrentIndex(mode);
            ok->click();
            QTRY_VERIFY(observer.refreshes > previousRefreshes);
            QVERIFY(actions->action(QStringLiteral("mupdf_theme_") + themes.at(mode).toLower())->isChecked());
            QCOMPARE(general.readEntry("ReadingTheme", QStringLiteral("Default")), themes.at(mode));
            QVERIFY(document.isHistoryClean());
            previousMode = mode;
        }
        const int previousRefreshes = observer.refreshes;
        actions->action(QStringLiteral("mupdf_theme_dark"))->trigger();
        QTRY_VERIFY(observer.refreshes > previousRefreshes);
        document.closeDocument();
        QCOMPARE(Okular::SettingsCore::paperColor(), originalPaper);
#if MU_OKULAR_PAGE_PLACEHOLDER_COLOR
        QCOMPARE(other.page(0)->placeholderColor(), QColor(32, 33, 36));
#endif
        other.closeDocument();
        QCOMPARE(Okular::SettingsCore::changeColors(), originalChangeColors);
        QCOMPARE(Okular::SettingsCore::renderMode(), originalRenderMode);
        QCOMPARE(Okular::SettingsCore::paperColor(), originalPaper);
        QVERIFY(!actions->action(QStringLiteral("mupdf_theme"))->isEnabled());
        QVERIFY(!actions->action(QStringLiteral("mupdf_theme_toggle_dusk"))->isEnabled());
        QVERIFY(!actions->action(QStringLiteral("mupdf_theme_toggle_dark"))->isEnabled());
        QVERIFY(!actions->action(QStringLiteral("mupdf_theme_toggle_twilight"))->isEnabled());
        QFile invalid(m_root.filePath(QStringLiteral("invalid.pdf")));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("invalid PDF");
        invalid.close();
        Okular::Document failed(nullptr);
        QCOMPARE(failed.openDocument(invalid.fileName(),
                                     QUrl::fromLocalFile(invalid.fileName()),
                                     QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))),
                 Okular::Document::OpenError);
        QCOMPARE(Okular::SettingsCore::changeColors(), originalChangeColors);
        QCOMPARE(Okular::SettingsCore::renderMode(), originalRenderMode);
        QCOMPARE(Okular::SettingsCore::paperColor(), originalPaper);
    }

    void ocrMenuModels_data()
    {
        QTest::addColumn<QStringList>("files");
        QTest::addColumn<bool>("shared");
        QTest::newRow("empty") << QStringList() << false;
        QTest::newRow("support-only") << QStringList { "osd.traineddata", "equ.traineddata" } << false;
        QTest::newRow("language") << QStringList { "test.traineddata" } << false;
        QTest::newRow("shared-empty") << QStringList() << true;
        QTest::newRow("shared-support-only") << QStringList { "osd.traineddata", "equ.traineddata" } << true;
        QTest::newRow("shared-language") << QStringList { "test.traineddata" } << true;
    }

    void ocrMenuModels()
    {
        QFETCH(QStringList, files);
        QFETCH(bool, shared);
        QTemporaryDir models;
        QVERIFY(models.isValid());
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup advanced(config, QStringLiteral("Advanced"));
        advanced.writeEntry("TessDataDirectories", QStringList { models.path() });
        config->sync();
        auto restore = qScopeGuard([&] {
            advanced.deleteEntry("TessDataDirectories");
            config->sync();
        });
        Okular::Document document(nullptr);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        auto* client = document.guiClient();
        QVERIFY(client);
        auto* actions = client->actionCollection();
        auto* ocrMenu = actions->action(QStringLiteral("mupdf_ocr"));
        QVERIFY(ocrMenu);
        auto* menu = ocrMenu->menu();
        QVERIFY(menu);
        QSignalSpy refreshed(menu, &QMenu::aboutToShow);
        QMainWindow window;
        KXMLGUIBuilder builder(&window);
        KXMLGUIFactory factory(&builder);
        TestGuiClient host;
        factory.addClient(&host);
        auto cleanup = qScopeGuard([&] {
            factory.removeClient(client);
            factory.removeClient(&host);
        });
        if (shared) {
            factory.addClient(client);
            auto* tools = qobject_cast<QMenu*>(factory.container(QStringLiteral("tools"), &host));
            QVERIFY(tools);
            QTRY_COMPARE(tools->actions().size(), 5);
            for (auto* candidate : factory.clients()) {
                if (candidate->xmlFile() == QStringLiteral("mupdfngui.rc"))
                    actions = candidate->actionCollection();
            }
            QVERIFY(actions != client->actionCollection());
            menu = actions->action(QStringLiteral("mupdf_ocr"))->menu();
        }
        // Refresh on opening, including models installed/removed while Okular is running.
        for (const bool installed : { true, false }) {
            for (const QString& file : files) {
                QFile model(models.filePath(file));
                if (installed)
                    QVERIFY(model.open(QIODevice::WriteOnly));
                else
                    QVERIFY(model.remove());
            }
            QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow"));
            QCOMPARE(refreshed.count(), installed ? 1 : 2);
            const bool hasModels =
                !Mu::Generator::Config::installedOcrModels({ QStringLiteral(TESSDATA_DIR), models.path() }).isEmpty();
            QList<QAction*> visible;
            for (QAction* action : menu->actions()) {
                if (action->isVisible())
                    visible.append(action);
            }
            QCOMPARE(visible.size(), hasModels ? 3 : 1);
            QCOMPARE(actions->action(QStringLiteral("mupdf_ocr_toggle"))->isEnabled(), hasModels);
            for (QAction* action : visible)
                QCOMPARE(action->isEnabled(), hasModels);
            if (!hasModels)
                QCOMPARE(visible.front()->text(),
                         QStringLiteral("No OCR models available. Please install a Tesseract language model."));
        }
    }

    void ocrMenu_data()
    {
        QTest::addColumn<QString>("initialMode");
        QTest::addColumn<QString>("actionName");
        QTest::addColumn<QString>("requestedMode");
        QTest::newRow("off") << QStringLiteral("Always") << QStringLiteral("mupdf_ocr_off") << QStringLiteral("Never");
        QTest::newRow("automatic") << QStringLiteral("Never") << QStringLiteral("mupdf_ocr_auto")
                                   << QStringLiteral("Five");
        QTest::newRow("on") << QStringLiteral("Five") << QStringLiteral("mupdf_ocr_on") << QStringLiteral("Always");
        QTest::newRow("automatic-twenty")
            << QStringLiteral("Twenty") << QStringLiteral("mupdf_ocr_auto") << QStringLiteral("Five");
        QTest::newRow("toggle-from-off") << QStringLiteral("Never") << QStringLiteral("mupdf_ocr_toggle")
                                         << QStringLiteral("Always");
        QTest::newRow("toggle-from-five")
            << QStringLiteral("Five") << QStringLiteral("mupdf_ocr_toggle") << QStringLiteral("Always");
        QTest::newRow("toggle-from-twenty")
            << QStringLiteral("Twenty") << QStringLiteral("mupdf_ocr_toggle") << QStringLiteral("Always");
        QTest::newRow("toggle-from-on") << QStringLiteral("Always") << QStringLiteral("mupdf_ocr_toggle")
                                        << QStringLiteral("Never");
    }

    void menuBarOrder_data()
    {
        QTest::addColumn<int>("teardown");
        QTest::newRow("close-after-rebuild") << 0;
        QTest::newRow("close-before-rebuild") << 1;
        QTest::newRow("destroy-factory-before-rebuild") << 2;
        QTest::newRow("close-during-unplug") << 3;
    }

    void menuBarOrder()
    {
        QFETCH(int, teardown);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        config->sync();
        QMainWindow window;
        KXMLGUIBuilder builder(&window);
        auto factory = std::make_unique<KXMLGUIFactory>(&builder);
        TestMenuClient shell({ "file", "view", "settings", "help" }, true);
        TestMenuClient firstPart({ "edit", "go", "bookmarks", "tools" });
        TestMenuClient secondPart({ "edit", "go", "bookmarks", "tools" });
        Okular::Document first(nullptr), second(nullptr);
        auto cleanup = qScopeGuard([&] {
            if (!factory)
                return;
            for (auto* document : { &first, &second }) {
                if (auto* client = document->guiClient(); client && client->factory())
                    factory->removeClient(client);
            }
            factory->removeClient(&firstPart);
            factory->removeClient(&secondPart);
            factory->removeClient(&shell);
        });
        factory->addClient(&shell);
        factory->addClient(&firstPart);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        const auto mime = QMimeDatabase().mimeTypeForFile(path);
        QCOMPARE(first.openDocument(path, QUrl::fromLocalFile(path), mime), Okular::Document::OpenSuccess);
        factory->addClient(first.guiClient());
        const auto tools = [&]() {
            return qobject_cast<QMenu*>(factory->container(QStringLiteral("tools"), nullptr));
        };
        QTRY_VERIFY(tools() && tools()->actions().size() == 5);
        QPointer<QAction> theme = tools()->actions().at(3);
        const auto menuOrder = [&] {
            QStringList order;
            if (auto* bar = qobject_cast<QMenuBar*>(factory->container(QStringLiteral("menubar"), nullptr, true))) {
                for (auto* action : bar->actions())
                    order.append(action->menu()->objectName());
            }
            return order;
        };
        const QStringList expected { "file", "view", "edit", "go", "bookmarks", "tools", "settings", "help" };
        QCOMPARE(menuOrder(), expected);
        factory->removeClient(&firstPart);
        factory->addClient(&secondPart);
        QCOMPARE(second.openDocument(path, QUrl::fromLocalFile(path), mime), Okular::Document::OpenSuccess);
        factory->addClient(second.guiClient());
        QTRY_VERIFY(tools() && tools()->actions().contains(theme));
        QCOMPARE(menuOrder(), expected);
        for (int index = 0; index < 6; ++index) {
            auto* oldPart = index % 2 == 0 ? &secondPart : &firstPart;
            auto* newPart = index % 2 == 0 ? &firstPart : &secondPart;
            factory->removeClient(oldPart);
            factory->addClient(newPart);
            QTRY_VERIFY(tools() && tools()->actions().contains(theme));
            QCOMPARE(menuOrder(), expected);
            QCOMPARE(tools()->actions().size(), 5);
            QCOMPARE(tools()->actions().front(), newPart->actionCollection()->action(QStringLiteral("tools")));
        }
        // Leave a rebuild queued while removing the last document or GUI factory.
        if (teardown == 3) {
            connect(factory.get(), &KXMLGUIFactory::clientRemoved, &window, [&](KXMLGUIClient* client) {
                if (client->xmlFile() == QStringLiteral("mupdfngui.rc")) {
                    factory->removeClient(first.guiClient());
                    factory->removeClient(second.guiClient());
                }
            });
        }
        factory->removeClient(&secondPart);
        factory->addClient(&firstPart);
        if (teardown == 0)
            QTRY_VERIFY(tools() && tools()->actions().contains(theme));
        if (teardown == 2) {
            factory.reset();
        } else {
            factory->removeClient(first.guiClient());
            factory->removeClient(second.guiClient());
        }
        QVERIFY(theme.isNull());
        QCoreApplication::processEvents();
        if (factory) {
            QVERIFY(tools());
            QCOMPARE(tools()->actions().size(), 1);
            QCOMPARE(menuOrder(), expected);
        }
        first.closeDocument();
        second.closeDocument();
    }

    void sharedWindowMenus_data()
    {
        QTest::addColumn<int>("firstClosed");
        QTest::addColumn<QString>("secondPath");
        QTest::addColumn<QString>("initialMode");
        QTest::newRow("close-owner") << 0 << QStringLiteral(TEST_PDF_PATH) << QStringLiteral("Five");
        QTest::newRow("close-newer") << 1 << QStringLiteral(TEST_PDF_PATH) << QStringLiteral("Five");
        QTest::newRow("mixed-formats") << 0 << QStringLiteral(TEST_EPUB_PATH) << QStringLiteral("Five");
        QTest::newRow("close-owner-twenty") << 0 << QStringLiteral(TEST_PDF_PATH) << QStringLiteral("Twenty");
        QTest::newRow("close-owner-off") << 0 << QStringLiteral(TEST_PDF_PATH) << QStringLiteral("Never");
    }

    void sharedWindowMenus()
    {
        QFETCH(int, firstClosed);
        QFETCH(QString, secondPath);
        QFETCH(QString, initialMode);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup ocr(config, QStringLiteral("OCR"));
        ocr.writeEntry("OcrTriggerMode", initialMode);
        config->sync();
        QMainWindow window, otherWindow;
        KXMLGUIBuilder builder(&window), otherBuilder(&otherWindow);
        KXMLGUIFactory factory(&builder), otherFactory(&otherBuilder);
        TestGuiClient host, otherHost;
        factory.addClient(&host);
        otherFactory.addClient(&otherHost);
        std::unique_ptr<Okular::Document> documents[3];
        KXMLGUIClient* clients[3] = { };
        auto cleanup = qScopeGuard([&] {
            for (int index = 0; index < 3; ++index) {
                if (documents[index] && clients[index] && clients[index]->factory())
                    clients[index]->factory()->removeClient(clients[index]);
            }
            factory.removeClient(&host);
            otherFactory.removeClient(&otherHost);
        });
        const QStringList paths { QStringLiteral(TEST_PDF_PATH), secondPath, QStringLiteral(TEST_PDF_PATH) };
        for (int index = 0; index < 3; ++index) {
            documents[index] = std::make_unique<Okular::Document>(nullptr);
            const auto& path = paths.at(index);
            QCOMPARE(
                documents[index]->openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                Okular::Document::OpenSuccess);
            clients[index] = documents[index]->guiClient();
            documents[index]->reparseConfig();
            (index == 2 ? otherFactory : factory).addClient(clients[index]);
        }
        QPointer<QMenu> tools = qobject_cast<QMenu*>(factory.container(QStringLiteral("tools"), &host));
        auto* otherTools = qobject_cast<QMenu*>(otherFactory.container(QStringLiteral("tools"), &otherHost));
        QVERIFY(tools);
        QVERIFY(otherTools);
        QTRY_COMPARE(tools->actions().size(), 5);
        QTRY_COMPARE(otherTools->actions().size(), 5);
        auto* theme = tools->actions().at(3);
        QVERIFY(theme != otherTools->actions().at(3));
        for (auto* client : clients)
            QVERIFY(client->actionCollection()->associatedWidgets().isEmpty());
        // Simulate the shell switching parts without unregistering generators.
        TestGuiClient part;
        factory.addClient(&part);
        factory.removeClient(&part);
        QTRY_COMPARE(tools->actions().size(), 5);
        // A tab switch rebuilds Tools along with the part. Reacquire its menu;
        // the shared rows must still follow the part's after the queued merge.
        for (int index = 0; index < 2; ++index) {
            factory.removeClient(&host);
            factory.addClient(&host);
            QTRY_VERIFY((tools = qobject_cast<QMenu*>(factory.container(QStringLiteral("tools"), &host)))
                        && tools->actions().size() == 5);
            QCOMPARE(tools->actions().front(), host.actionCollection()->action(QStringLiteral("previous")));
            QCOMPARE(tools->actions().at(2)->objectName(), QStringLiteral("mupdf_ocr"));
            QCOMPARE(tools->actions().at(3), theme);
            QCOMPARE(tools->actions().back()->objectName(), QStringLiteral("mupdf_manage_certificates"));
        }
        QAction* toggle = nullptr;
        for (auto* candidate : factory.clients()) {
            if (candidate->xmlFile() == QStringLiteral("mupdfngui.rc"))
                toggle = candidate->actionCollection()->action(QStringLiteral("mupdf_ocr_toggle"));
        }
        QVERIFY(toggle);
        const bool checkToggle = toggle->isEnabled() && secondPath == QStringLiteral(TEST_PDF_PATH);
        if (checkToggle) {
            toggle->trigger();
            config->reparseConfiguration();
            QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), QStringLiteral("Always"));
        }
        factory.removeClient(clients[firstClosed]);
        documents[firstClosed]->closeDocument();
        documents[firstClosed].reset();
        QTRY_COMPARE(tools->actions().size(), 5);
        if (checkToggle) {
            toggle->trigger();
            config->reparseConfiguration();
            QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), initialMode);
        }
        QCOMPARE(tools->actions().size(), 5);
        QCOMPARE(tools->actions().at(3), theme);
        const int remaining = 1 - firstClosed;
        auto* source = clients[remaining]->actionCollection()->action(QStringLiteral("mupdf_theme_dark"));
        QSignalSpy triggered(source, &QAction::triggered);
        theme->menu()->actions().back()->trigger();
        QCOMPARE(triggered.count(), 1);
        // Removing and re-adding a cached client must restore one presentation.
        if (checkToggle)
            toggle->trigger();
        factory.removeClient(clients[remaining]);
        QCOMPARE(tools->actions().size(), 1);
        QCOMPARE(otherTools->actions().size(), 5);
        if (checkToggle) {
            // The registration also retains history while no GUI is attached.
            auto* detachedToggle = clients[remaining]->actionCollection()->action(QStringLiteral("mupdf_ocr_toggle"));
            detachedToggle->trigger();
            config->reparseConfiguration();
            QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), initialMode);
            detachedToggle->trigger();
        }
        factory.addClient(clients[remaining]);
        QTRY_COMPARE(tools->actions().size(), 5);
        if (checkToggle) {
            for (auto* candidate : factory.clients()) {
                if (candidate->xmlFile() == QStringLiteral("mupdfngui.rc"))
                    candidate->actionCollection()->action(QStringLiteral("mupdf_ocr_toggle"))->trigger();
            }
            config->reparseConfiguration();
            QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), initialMode);
        }
        factory.removeClient(clients[remaining]);
        documents[remaining]->closeDocument();
        documents[remaining].reset();
        QCOMPARE(tools->actions().size(), 1);
        QCOMPARE(otherTools->actions().size(), 5);
    }

    void closesDocumentDuringMenuAction_data()
    {
        QTest::addColumn<QString>("actionName");
        QTest::addColumn<bool>("keepOtherDocument");
        QTest::newRow("submenu-last-document") << QStringLiteral("mupdf_theme_dark") << false;
        QTest::newRow("submenu-surviving-document") << QStringLiteral("mupdf_theme_dark") << true;
        QTest::newRow("toggle-last-document") << QStringLiteral("mupdf_theme_toggle_dark") << false;
        QTest::newRow("toggle-surviving-document") << QStringLiteral("mupdf_theme_toggle_dark") << true;
    }

    void closesDocumentDuringMenuAction()
    {
        QFETCH(QString, actionName);
        QFETCH(bool, keepOtherDocument);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        config->sync();
        Okular::Document first(nullptr), second(nullptr);
        QMainWindow window;
        KXMLGUIBuilder builder(&window);
        KXMLGUIFactory factory(&builder);
        TestGuiClient host;
        factory.addClient(&host);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        for (auto* document : { &first, &second }) {
            if (document == &second && !keepOtherDocument)
                break;
            QCOMPARE(document->openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                     Okular::Document::OpenSuccess);
            factory.addClient(document->guiClient());
        }
        auto cleanup = qScopeGuard([&] {
            for (auto* document : { &first, &second }) {
                if (auto* client = document->guiClient(); client && client->factory())
                    factory.removeClient(client);
            }
            factory.removeClient(&host);
        });
        auto* tools = qobject_cast<QMenu*>(factory.container(QStringLiteral("tools"), &host));
        QVERIFY(tools);
        QTRY_COMPARE(tools->actions().size(), 5);
        QPointer<QAction> proxy;
        for (auto* client : factory.clients()) {
            if (client->xmlFile() == QStringLiteral("mupdfngui.rc"))
                proxy = client->actionCollection()->action(actionName);
        }
        QVERIFY(proxy);
        auto* client = first.guiClient();
        auto* source = client->actionCollection()->action(actionName);
        QSignalSpy triggered(source, &QAction::triggered);
        connect(source, &QAction::triggered, &factory, [&factory, client] { factory.removeClient(client); });
        proxy->trigger();
        QCOMPARE(triggered.count(), 1);
        QVERIFY(!client->factory());
        QTRY_COMPARE(tools->actions().size(), keepOtherDocument ? 5 : 1);
        if (keepOtherDocument) {
            QVERIFY(proxy);
            QSignalSpy otherTriggered(second.guiClient()->actionCollection()->action(actionName), &QAction::triggered);
            proxy->trigger();
            QCOMPARE(otherTriggered.count(), 1);
            QCOMPARE(triggered.count(), 1);
        } else {
            QVERIFY(proxy.isNull());
        }
    }

    void ocrMenu()
    {
        if (Mu::Generator::Config::installedOcrModels({ QStringLiteral(TESSDATA_DIR) }).isEmpty())
            QSKIP("OCR menu modes require an installed language model");
        QFETCH(QString, initialMode);
        QFETCH(QString, actionName);
        QFETCH(QString, requestedMode);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup ocr(config, QStringLiteral("OCR"));
        ocr.writeEntry("OcrTriggerMode", initialMode);
        config->sync();
        Okular::Document document(nullptr);
        const QString path = QStringLiteral(TEST_PDF_PATH);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        auto* client = document.guiClient();
        QVERIFY(client);
        QMainWindow window;
        KXMLGUIBuilder builder(&window);
        KXMLGUIFactory factory(&builder);
        TestGuiClient host;
        factory.addClient(&host);
        factory.addClient(client);
        auto cleanup = qScopeGuard([&] {
            factory.removeClient(client);
            factory.removeClient(&host);
        });
        auto* tools = qobject_cast<QMenu*>(factory.container(QStringLiteral("tools"), &host));
        QVERIFY(tools);
        QTRY_COMPARE(tools->actions().size(), 5);
        QVERIFY(tools->actions().at(1)->isSeparator());
        KXMLGUIClient* presentation = nullptr;
        for (auto* candidate : factory.clients()) {
            if (candidate->xmlFile() == QStringLiteral("mupdfngui.rc"))
                presentation = candidate;
        }
        QVERIFY(presentation);
        auto* certificates = presentation->actionCollection()->action(QStringLiteral("mupdf_manage_certificates"));
        QVERIFY(certificates);
        QCOMPARE(tools->actions().back(), certificates);
        QCOMPARE(certificates->text(), QStringLiteral("Manage Certificates…"));
        QCOMPARE(tools->actions().at(3), presentation->actionCollection()->action(QStringLiteral("mupdf_theme")));
        auto* menu = tools->actions().at(2)->menu();
        QVERIFY(menu);
        QCOMPARE(menu->title(), QStringLiteral("MuPDF OCR"));
        QCOMPARE(menu->actions().size(), 4);
        QVERIFY(!menu->actions().back()->isVisible());
        QCOMPARE(client->actionCollection()->actions().count(), 14);
        for (QAction* registered : presentation->actionCollection()->actions())
            QCOMPARE(KActionCollection::isShortcutsConfigurable(registered),
                     registered->objectName() == QStringLiteral("mupdf_ocr_toggle")
                         || registered->objectName().startsWith(QStringLiteral("mupdf_theme_toggle_")));
        QCOMPARE(menu->actions().at(1)->text(), QStringLiteral("Automatic"));
        const QStringList modes { "Never", "Five", "Always" };
        const QStringList labels { "Off", "Automatic", "On" };
        // KDE may add access-key markers when the menu is first shown.
        for (int i = 0; i < labels.size(); ++i)
            menu->actions().at(i)->setText('&' + labels.at(i));
        const QString initialMenuMode = initialMode == QStringLiteral("Twenty") ? QStringLiteral("Five") : initialMode;
        for (int i = 0; i < modes.size(); ++i)
            QCOMPARE(menu->actions().at(i)->isChecked(), modes.at(i) == initialMenuMode);
        auto* action = presentation->actionCollection()->action(actionName);
        QVERIFY(action);
        QSignalSpy notices(&document, &Okular::Document::notice);
        const auto resetShortcut = [&] {
            action->setShortcut({ });
            presentation->actionCollection()->writeSettings();
        };
        auto clearShortcut = qScopeGuard(resetShortcut);
        if (actionName == QStringLiteral("mupdf_ocr_toggle")) {
            QVERIFY(KActionCollection::isShortcutsConfigurable(action));
            const QKeySequence shortcut(Qt::CTRL | Qt::ALT | Qt::Key_F9);
            action->setShortcut(shortcut);
            presentation->actionCollection()->writeSettings();
            action->setShortcut({ });
            factory.refreshActionProperties();
            QCOMPARE(action->shortcut(), shortcut);
            window.show();
            window.activateWindow();
            QTRY_VERIFY(window.isActiveWindow());
            QSignalSpy triggered(action, &QAction::triggered);
            QTest::keyClick(&window, Qt::Key_F9, Qt::ControlModifier | Qt::AltModifier);
            QTRY_COMPARE(triggered.count(), 1);
        } else {
            action->trigger();
        }
        config->reparseConfiguration();
        QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), requestedMode);
        QCOMPARE(notices.count(), 1);
        QCOMPARE(notices.front().at(0).toString(),
                 QStringLiteral("MuPDF OCR: %1").arg(labels.at(modes.indexOf(requestedMode))));
        QCOMPARE(notices.front().at(1).toInt(), 1500);
        for (int i = 0; i < modes.size(); ++i)
            QCOMPARE(menu->actions().at(i)->isChecked(), modes.at(i) == requestedMode);

        if (actionName == QStringLiteral("mupdf_ocr_toggle")) {
            int expectedNotices = 1;
            for (const auto& expectedMode : { initialMode, requestedMode, initialMode }) {
                QTest::keyClick(&window, Qt::Key_F9, Qt::ControlModifier | Qt::AltModifier);
                ++expectedNotices;
                QTRY_COMPARE(notices.count(), expectedNotices);
                config->reparseConfiguration();
                QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), expectedMode);
                const QString menuMode =
                    expectedMode == QStringLiteral("Twenty") ? QStringLiteral("Five") : expectedMode;
                for (int i = 0; i < modes.size(); ++i)
                    QCOMPARE(menu->actions().at(i)->isChecked(), modes.at(i) == menuMode);
                QCOMPARE(notices.back().at(0).toString(),
                         QStringLiteral("MuPDF OCR: %1").arg(labels.at(modes.indexOf(menuMode))));
            }
            QCOMPARE(notices.count(), 4);
        }

        // Changes from the configuration dialog refresh the same menu state.
        ocr.writeEntry("OcrTriggerMode", initialMode);
        config->sync();
        document.reparseConfig();
        for (int i = 0; i < modes.size(); ++i)
            QCOMPARE(menu->actions().at(i)->isChecked(), modes.at(i) == initialMenuMode);
        if (actionName == QStringLiteral("mupdf_ocr_toggle")) {
            // A settings change replaces the mode remembered by the next toggle.
            ocr.writeEntry("OcrTriggerMode", "Twenty");
            config->sync();
            document.reparseConfig();
            for (const auto& expected : { QStringLiteral("Always"), QStringLiteral("Twenty") }) {
                action->trigger();
                config->reparseConfiguration();
                QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), expected);
            }
            client->actionCollection()->action(QStringLiteral("mupdf_ocr_off"))->trigger();
            for (const auto& expected : { QStringLiteral("Always"), QStringLiteral("Never") }) {
                action->trigger();
                config->reparseConfiguration();
                QCOMPARE(ocr.readEntry("OcrTriggerMode", QStringLiteral("Five")), expected);
            }
        }
        resetShortcut();
        clearShortcut.dismiss();
        factory.removeClient(client);
        QCOMPARE(tools->actions().size(), 1);
        factory.removeClient(&host);
        cleanup.dismiss();
    }

    void preservesNativeText_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("withImage");
        QTest::addColumn<bool>("missingModel");
        QTest::newRow("mixed-page") << QStringLiteral("Always") << true << false;
        QTest::newRow("text-only-page") << QStringLiteral("Always") << false << false;
        QTest::newRow("recognition-failure") << QStringLiteral("Always") << true << true;
        QTest::newRow("automatic-five") << QStringLiteral("Five") << true << false;
        QTest::newRow("automatic-twenty") << QStringLiteral("Twenty") << true << false;
    }

    void preservesNativeText()
    {
        QFETCH(QString, mode);
        QFETCH(bool, withImage);
        const QString nativeText =
            mode == QStringLiteral("Always") ? QStringLiteral("NATIVEONLY") : QStringLiteral("OK");
        QFETCH(bool, missingModel);
        if (!QFile::exists(QStringLiteral(TESSDATA_DIR "/eng.traineddata")))
            QSKIP("English traineddata is not installed");
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        config->reparseConfiguration();
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup ocr(config, QStringLiteral("OCR"));
        ocr.writeEntry("OcrLanguage", missingModel ? "missingocrmodel.traineddata" : "eng.traineddata");
        ocr.writeEntry("OcrTriggerMode", mode);
        ocr.writeEntry("OcrQuality", "Speed");
        ocr.writeEntry("OcrDebounceMs", 1000);
        ocr.writeEntry("OcrNotify", true);
        config->sync();
        const QString path = m_root.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".pdf");
        {
            QPdfWriter pdf(path);
            pdf.setTitle(QString::fromLatin1(QTest::currentDataTag()));
            pdf.setResolution(150);
            QPainter painter(&pdf);
            QFont font(QStringLiteral("DejaVu Sans"));
            font.setPixelSize(70);
            painter.setFont(font);
            painter.drawText(QPoint(100, 650), nativeText);
            if (withImage) {
                QImage scan(1000, 400, QImage::Format_RGB32);
                scan.fill(Qt::white);
                QPainter imagePainter(&scan);
                imagePainter.setFont(font);
                imagePainter.drawText(scan.rect(), Qt::AlignCenter, QStringLiteral("RASTERONLY"));
                imagePainter.end();
                painter.drawImage(QRect(100, 100, 1000, 400), scan);
            }
        }
        Okular::DocumentObserver observer;
        Okular::Document document(nullptr);
        QSignalSpy notices(&document, &Okular::Document::notice);
        QSignalSpy warnings(&document, &Okular::Document::warning);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        document.setVisiblePageRects({ new Okular::VisiblePageRect(0, { 0, 0, 1, 1 }) });
        document.requestTextPage(0);
        // Okular may insert layout spaces between kerned native glyphs.
        const auto text = [&] {
            return document.page(0)->text().remove(' ').remove('\n');
        };
        QTRY_VERIFY_WITH_TIMEOUT(document.page(0)->hasTextPage(), 900);
        QVERIFY(text().contains(nativeText));
        QVERIFY(!text().contains(QStringLiteral("RASTERONLY")));
        if (missingModel) {
            QTRY_VERIFY_WITH_TIMEOUT(!warnings.empty(), 10000);
        } else {
            const auto completed = [&] {
                return std::any_of(notices.begin(), notices.end(), [](const auto& notice) {
                    return notice.front().toString().contains(QStringLiteral("OCR completed"));
                });
            };
            if (withImage)
                QTRY_VERIFY_WITH_TIMEOUT(completed(), 10000);
            else
                QTest::qWait(1500);
            QCOMPARE(text().count(QStringLiteral("RASTERONLY")), withImage ? 1 : 0);
        }
        QCOMPARE(text().count(nativeText), 1);
        document.closeDocument();
        if (!missingModel) {
            // Race cached OCR delivery against Okular's threaded text extraction.
            ocr.writeEntry("OcrDebounceMs", 0);
            config->sync();
            notices.clear();
            QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                     Okular::Document::OpenSuccess);
            document.reparseConfig();
            document.addObserver(&observer);
            document.setVisiblePageRects({ new Okular::VisiblePageRect(0, { 0, 0, 1, 1 }) });
            document.requestPixmaps(
                { new Okular::PixmapRequest(&observer, 0, 600, 800, 1, 0, Okular::PixmapRequest::Asynchronous) });
            QTRY_VERIFY_WITH_TIMEOUT(document.page(0)->hasPixmap(&observer), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(document.page(0)->hasTextPage(), 10000);
            if (withImage)
                QTRY_VERIFY_WITH_TIMEOUT(text().contains(QStringLiteral("RASTERONLY")), 10000);
            else
                QTest::qWait(1500);
            QCOMPARE(text().count(nativeText), 1);
            QVERIFY(std::none_of(notices.begin(), notices.end(), [](const auto& notice) {
                return notice.front().toString().contains(QStringLiteral("Running OCR"));
            }));
            document.closeDocument();
            document.removeObserver(&observer);
        }
    }

    void recognizesVisiblePageWithoutSelection_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("automatic") << QStringLiteral("Five") << true;
        QTest::newRow("always") << QStringLiteral("Always") << true;
        QTest::newRow("disabled") << QStringLiteral("Never") << false;
    }

    void recognizesVisiblePageWithoutSelection()
    {
        QFETCH(QString, mode);
        QFETCH(bool, enabled);
        if (enabled && !QFile::exists(QStringLiteral(TESSDATA_DIR "/eng.traineddata")))
            QSKIP("English traineddata is not installed");

        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        config->reparseConfiguration();
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup ocr(config, QStringLiteral("OCR"));
        ocr.writeEntry("OcrLanguage", "eng.traineddata");
        ocr.writeEntry("OcrTriggerMode", "Never");
        ocr.writeEntry("OcrQuality", "Speed");
        ocr.writeEntry("OcrDebounceMs", 100);
        config->sync();

        // Rasterize the words so the PDF has no native text to select.
        QImage scan(1000, 400, QImage::Format_RGB32);
        scan.fill(Qt::white);
        QFont font(QStringLiteral("DejaVu Sans"));
        font.setPixelSize(70);
        QVERIFY2(QFontMetrics(font).inFont(QChar('H')), "OCR fixture requires a Latin font, such as DejaVu Sans");
        QPainter scanPainter(&scan);
        scanPainter.setFont(font);
        scanPainter.drawText(scan.rect(), Qt::AlignCenter, QStringLiteral("HELLO VIEWPORT"));
        scanPainter.end();
        const QString path = m_root.filePath(mode + QStringLiteral(".pdf"));
        {
            QPdfWriter pdf(path);
            pdf.setResolution(150);
            QPainter painter(&pdf);
            for (int page = 0; page < 2; ++page) {
                if (page != 0)
                    QVERIFY(pdf.newPage());
                painter.drawImage(QRect(100, 100, 1000, 400), scan);
            }
        }

        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.reparseConfig();
        QCOMPARE(document.pages(), 2u);
        for (int page = 0; page < 2; ++page) {
            QVERIFY(!document.page(page)->hasTextPage());
            document.setVisiblePageRects({ new Okular::VisiblePageRect(page, { 0, 0, 1, 1 }) });
            if (page == 0) {
                const QString actionName = mode == QStringLiteral("Five") ? QStringLiteral("mupdf_ocr_auto")
                    : mode == QStringLiteral("Always")                    ? QStringLiteral("mupdf_ocr_on")
                                                                          : QStringLiteral("mupdf_ocr_off");
                auto* action = document.guiClient()->actionCollection()->action(actionName);
                QVERIFY(action);
                action->trigger();
            }
            if (enabled) {
                QTRY_VERIFY_WITH_TIMEOUT(document.page(page)->hasTextPage(), 10000);
                QVERIFY(document.page(page)->text().contains(QStringLiteral("HELLO")));
            } else {
                QTest::qWait(500);
                QVERIFY(!document.page(page)->hasTextPage());
            }
        }
        // Reopening exercises observer removal and registration on the same backend.
        document.closeDocument();
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        document.setVisiblePageRects({ new Okular::VisiblePageRect(0, { 0, 0, 1, 1 }) });
        if (enabled)
            QTRY_VERIFY_WITH_TIMEOUT(document.page(0)->hasTextPage(), 10000);
        document.closeDocument();
    }
};

QTEST_MAIN(TestGeneratorOcr)
#include "test_ocr.moc"
