// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QMimeDatabase>
#include <QPainter>
#include <QPdfWriter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <okular/core/document.h>
#include <okular/core/generator.h>
#include <okular/core/observer.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

#include <algorithm>

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
        ocr.writeEntry("OcrTriggerMode", mode);
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
