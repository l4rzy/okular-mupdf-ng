// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "generator/config/settings.hpp"
#include "mupdfngsettings.h"

class TestGeneratorConfig : public QObject {
    Q_OBJECT

private slots:

    void buildsDocumentSettings()
    {
        ::Mu::Generator::Config::RenderingSettings rendering { 4, 6, 2, false, 256LL * 1024 * 1024 };
        ::Mu::Generator::Config::EpubSettings epub { 99, -1, 99, QStringLiteral("Ym9keXt9") };

        const auto settings = ::Mu::Generator::Config::documentSettingsFor(rendering, epub, 0x112233);
        QCOMPARE(settings.graphicsAntialiasing, 4);
        QCOMPARE(settings.textAntialiasing, 6);
        QCOMPARE(settings.imageQuality, 2);
        QVERIFY(!settings.interpolateImages);
        QCOMPARE(settings.memoryCacheBytes, 256LL * 1024 * 1024);
        QCOMPARE(settings.paperColorRgb, 0x112233u);
        QCOMPARE(settings.epub.fontSize, 20);
        QCOMPARE(static_cast<int>(settings.epub.fontFamily), 0);
        QCOMPARE(static_cast<int>(settings.epub.pageSize), 3);
        QCOMPARE(QString::fromStdString(settings.epub.customCssBase64), QStringLiteral("Ym9keXt9"));
    }

    void distinguishesRenderingOutputChanges()
    {
        const ::Mu::Generator::Config::RenderingSettings original { 4, 6, 1, true, 64LL * 1024 * 1024 };
        auto memoryOnly = original;
        memoryOnly.memoryCacheBytes = 128LL * 1024 * 1024;
        QVERIFY(!::Mu::Generator::Config::renderingOutputChanged(original, memoryOnly));

        auto antialiasing = original;
        antialiasing.graphicsAntialiasing = 8;
        QVERIFY(::Mu::Generator::Config::renderingOutputChanged(original, antialiasing));
    }

    void buildsOcrConfiguration()
    {
        ::Mu::Generator::Config::OcrSettings settings;
        settings.language = QStringLiteral("eng");
        settings.dpi = 300;
        settings.force = true;
        settings.notify = true;
        const auto target = ::Mu::Generator::Config::ocrTargetFor(QStringLiteral("hash"), settings);
        const auto config = ::Mu::Generator::Config::ocrConfigFor(target, 12, 144, 144, settings);

        QCOMPARE(config.documentHash, QStringLiteral("hash"));
        QCOMPARE(config.language, QStringLiteral("eng"));
        QCOMPARE(config.pageCount, 12);
        QCOMPARE(config.dpi, 300);
        QVERIFY(config.force);
    }

    void disablesAutomaticOcrWhenNeverSelected()
    {
        const int originalTriggerMode = MuPDFSettings::ocrTriggerMode();
        MuPDFSettings::setOcrTriggerMode(MuPDFSettings::EnumOcrTriggerMode::Never);

        const auto settings = ::Mu::Generator::Config::readOcrSettings();
        MuPDFSettings::setOcrTriggerMode(originalTriggerMode);

        QVERIFY(!settings.force);
        QVERIFY(!settings.autoTrigger);
    }

    void disablesOcrTriggersWhenNoModelInstalled()
    {
        const QString originalLanguage = MuPDFSettings::ocrLanguage();
        const int originalTriggerMode = MuPDFSettings::ocrTriggerMode();
        MuPDFSettings::setOcrLanguage(QStringLiteral("-"));
        MuPDFSettings::setOcrTriggerMode(MuPDFSettings::EnumOcrTriggerMode::Always);

        const auto settings = ::Mu::Generator::Config::readOcrSettings();
        MuPDFSettings::setOcrLanguage(originalLanguage);
        MuPDFSettings::setOcrTriggerMode(originalTriggerMode);

        QCOMPARE(settings.language, QStringLiteral("-"));
        QVERIFY(!settings.force);
        QVERIFY(!settings.autoTrigger);
    }

    void disablesOcrWhenLanguageIsNotAModelFilename()
    {
        const QString originalLanguage = MuPDFSettings::ocrLanguage();
        const int originalTriggerMode = MuPDFSettings::ocrTriggerMode();
        MuPDFSettings::setOcrLanguage(QStringLiteral("deu"));
        MuPDFSettings::setOcrTriggerMode(MuPDFSettings::EnumOcrTriggerMode::Always);

        const auto settings = ::Mu::Generator::Config::readOcrSettings();
        MuPDFSettings::setOcrLanguage(originalLanguage);
        MuPDFSettings::setOcrTriggerMode(originalTriggerMode);

        QCOMPARE(settings.language, QStringLiteral("-"));
        QVERIFY(!settings.force);
        QVERIFY(!settings.autoTrigger);
    }

    void usesDefaultCertificateDatabasePerPreference()
    {
        const bool original = MuPDFSettings::useDefaultCertDB();
        MuPDFSettings::setUseDefaultCertDB(true);
        QVERIFY(::Mu::Generator::Config::usesDefaultCertificateDatabase());
        MuPDFSettings::setUseDefaultCertDB(false);
        QVERIFY(!::Mu::Generator::Config::usesDefaultCertificateDatabase());
        MuPDFSettings::setUseDefaultCertDB(original);
    }

    void normalizesTessdataDirectories()
    {
        const QStringList input {
            QStringLiteral("relative"),
            QStringLiteral("/usr/share/tessdata/"),
            QStringLiteral("/tmp/../cache/tessdata"),
            QStringLiteral("/usr/share/tessdata"),
            QStringLiteral("~/tessdata"),
            QStringLiteral("/"),
        };

        QCOMPARE(
            ::Mu::Generator::Config::normalizeTessDataDirectories(input),
            QStringList(
                { QStringLiteral("/usr/share/tessdata"), QStringLiteral("/cache/tessdata"), QStringLiteral("/") }));
    }
};

QTEST_GUILESS_MAIN(TestGeneratorConfig)

#include "test_config.moc"
