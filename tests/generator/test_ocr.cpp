// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KActionCollection>
#include <KConfigGroup>
#include <KSharedConfig>
#include <KXMLGUIBuilder>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <QAction>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QMainWindow>
#include <QMenu>
#include <QMimeDatabase>
#include <QPainter>
#include <QPdfWriter>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <okular/core/document.h>
#include <okular/core/generator.h>
#include <okular/core/observer.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

#include <algorithm>

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

class TestGeneratorOcr : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;

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
            QCOMPARE(action->isEnabled(), expected);
            for (QAction* registered : client->actionCollection()->actions())
                QCOMPARE(registered->isEnabled(),
                         registered->objectName() == QStringLiteral("mupdf_manage_certificates") || expected);
            document.reparseConfig();
            QCOMPARE(action->isEnabled(), expected);
            document.closeDocument();
            QVERIFY(!action->isEnabled());
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
            << QStringLiteral("Five") << QStringLiteral("mupdf_ocr_toggle") << QStringLiteral("Never");
        QTest::newRow("toggle-from-twenty")
            << QStringLiteral("Twenty") << QStringLiteral("mupdf_ocr_toggle") << QStringLiteral("Never");
        QTest::newRow("toggle-from-on") << QStringLiteral("Always") << QStringLiteral("mupdf_ocr_toggle")
                                        << QStringLiteral("Never");
    }

    void ocrMenu()
    {
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
        QCOMPARE(tools->actions().size(), 4);
        QVERIFY(tools->actions().at(1)->isSeparator());
        QCOMPARE(tools->actions().at(2)->icon().name(), QStringLiteral("document-scan"));
        auto* certificates = client->actionCollection()->action(QStringLiteral("mupdf_manage_certificates"));
        QVERIFY(certificates);
        QCOMPARE(tools->actions().back(), certificates);
        QCOMPARE(certificates->text(), QStringLiteral("Manage Certificates…"));
        auto* menu = tools->actions().at(2)->menu();
        QVERIFY(menu);
        QCOMPARE(menu->title(), QStringLiteral("MuPDF OCR"));
        QCOMPARE(menu->actions().size(), 3);
        QCOMPARE(client->actionCollection()->actions().count(), 6);
        for (QAction* registered : client->actionCollection()->actions())
            QCOMPARE(KActionCollection::isShortcutsConfigurable(registered),
                     registered->objectName() == QStringLiteral("mupdf_ocr_toggle"));
        QCOMPARE(menu->actions().at(1)->text(), QStringLiteral("Automatic"));
        const QStringList modes { "Never", "Five", "Always" };
        const QString initialMenuMode = initialMode == QStringLiteral("Twenty") ? QStringLiteral("Five") : initialMode;
        for (int i = 0; i < modes.size(); ++i)
            QCOMPARE(menu->actions().at(i)->isChecked(), modes.at(i) == initialMenuMode);
        auto* action = client->actionCollection()->action(actionName);
        QVERIFY(action);
        auto clearShortcut = qScopeGuard([&] {
            action->setShortcut({ });
            client->actionCollection()->writeSettings();
        });
        if (actionName == QStringLiteral("mupdf_ocr_toggle")) {
            QVERIFY(KActionCollection::isShortcutsConfigurable(action));
            const QKeySequence shortcut(Qt::CTRL | Qt::ALT | Qt::Key_F9);
            action->setShortcut(shortcut);
            client->actionCollection()->writeSettings();
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
        for (int i = 0; i < modes.size(); ++i)
            QCOMPARE(menu->actions().at(i)->isChecked(), modes.at(i) == requestedMode);

        // Changes from the configuration dialog refresh the same menu state.
        ocr.writeEntry("OcrTriggerMode", initialMode);
        config->sync();
        document.reparseConfig();
        for (int i = 0; i < modes.size(); ++i)
            QCOMPARE(menu->actions().at(i)->isChecked(), modes.at(i) == initialMenuMode);
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
