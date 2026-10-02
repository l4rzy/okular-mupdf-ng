// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KLocalizedString>
#include <QTest>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <array>
#include <cstdint>
#include <string_view>

#include "generator/config/certmanager/dialog_utils.hpp"
#include "generator/config/certmanager/self_signed_dialog.hpp"
#include "generator/config/settings.hpp"
#include "mupdfngsettings.h"
#include "shared/model/validation.hpp"

class TestGeneratorConfig : public QObject {
    Q_OBJECT

private slots:

    void initTestCase() { KLocalizedString::setApplicationDomain("okular_mupdfng"); }

    void selectsSigningKey_data()
    {
        using Mu::Plugin::Crypto::CertificateDatabase::SigningKey;
        QTest::addColumn<int>("index");
        QTest::addColumn<int>("key");
        QTest::newRow("RSA-2048") << 0 << static_cast<int>(SigningKey::Rsa2048);
        QTest::newRow("RSA-3072") << 1 << static_cast<int>(SigningKey::Rsa3072);
        QTest::newRow("RSA-4096") << 2 << static_cast<int>(SigningKey::Rsa4096);
        QTest::newRow("ECDSA-P256") << 3 << static_cast<int>(SigningKey::EcdsaP256);
    }

    void selectsSigningKey()
    {
        QFETCH(int, index);
        QFETCH(int, key);
        Mu::Generator::SelfSignedCertificateDialog dialog(QStringLiteral("test-db"));
        auto* combo = dialog.findChild<QComboBox*>(QStringLiteral("signingKey"));
        QVERIFY(combo);
        QCOMPARE(combo->count(), 4);
        QCOMPARE(dialog.certificateOptions().signingKey, Mu::Plugin::Crypto::CertificateDatabase::SigningKey::Rsa2048);
        combo->setCurrentIndex(index);
        QCOMPARE(static_cast<int>(dialog.certificateOptions().signingKey), key);
        QVERIFY(!combo->toolTip().isEmpty());
        for (const auto& line : combo->toolTip().split(QLatin1Char('\n')))
            QVERIFY(line.toUcs4().size() <= 80);
    }

    void formatsCertificateKeyAlgorithm()
    {
        const auto keyType = [](::Mu::Model::PublicKeyAlgorithm algorithm) {
            return static_cast<std::int32_t>(algorithm);
        };
        ::Mu::Model::Certificate certificate;
        certificate.publicKeyType = keyType(::Mu::Model::PublicKeyAlgorithm::Rsa);
        certificate.publicKeyStrength = 2048;
        QCOMPARE(::Mu::Generator::CertificateManager::formatKeyAlgorithm(certificate), QStringLiteral("RSA 2048"));

        certificate.publicKeyType = keyType(::Mu::Model::PublicKeyAlgorithm::Ec);
        certificate.publicKeyStrength = 256;
        QCOMPARE(::Mu::Generator::CertificateManager::formatKeyAlgorithm(certificate), QStringLiteral("EC 256"));

        certificate.publicKeyType = keyType(::Mu::Model::PublicKeyAlgorithm::Dsa);
        certificate.publicKeyStrength = 1024;
        QCOMPARE(::Mu::Generator::CertificateManager::formatKeyAlgorithm(certificate), QStringLiteral("DSA 1024"));

        // Missing strength keeps the bare algorithm name; unknown types are empty
        // so the dialog can present a localized fallback.
        certificate.publicKeyType = keyType(::Mu::Model::PublicKeyAlgorithm::Rsa);
        certificate.publicKeyStrength = 0;
        QCOMPARE(::Mu::Generator::CertificateManager::formatKeyAlgorithm(certificate), QStringLiteral("RSA"));

        certificate.publicKeyType = keyType(::Mu::Model::PublicKeyAlgorithm::Unknown);
        QVERIFY(::Mu::Generator::CertificateManager::formatKeyAlgorithm(certificate).isEmpty());

        // Out-of-range values from IPC must not fall through the switch.
        certificate.publicKeyType = 99;
        QVERIFY(::Mu::Generator::CertificateManager::formatKeyAlgorithm(certificate).isEmpty());
    }

    void buildsDocumentSettings()
    {
        ::Mu::Generator::Config::RenderingSettings rendering {
            4, 6, 2, false, 256LL * 1024 * 1024, ::Mu::Model::IdleTrimLevel::Aggressive
        };
        ::Mu::Generator::Config::EpubSettings epub { 99, -1, 99, QStringLiteral("Ym9keXt9") };

        const auto settings = ::Mu::Generator::Config::documentSettingsFor(rendering, epub, 0x112233);
        QCOMPARE(settings.graphicsAntialiasing, 4);
        QCOMPARE(settings.textAntialiasing, 6);
        QCOMPARE(settings.imageQuality, 2);
        QVERIFY(!settings.interpolateImages);
        QCOMPARE(settings.memoryCacheBytes, 256LL * 1024 * 1024);
        QCOMPARE(settings.idleTrimAggressiveness, ::Mu::Model::IdleTrimLevel::Aggressive);
        QCOMPARE(settings.paperColorRgb, 0x112233u);
        QCOMPARE(settings.epub.fontSize, 20);
        QCOMPARE(static_cast<int>(settings.epub.fontFamily), 0);
        QCOMPARE(static_cast<int>(settings.epub.pageSize), 3);
        QCOMPARE(QString::fromStdString(settings.epub.customCssBase64), QStringLiteral("Ym9keXt9"));
    }

    void distinguishesRenderingOutputChanges()
    {
        const ::Mu::Generator::Config::RenderingSettings original {
            4, 6, 1, true, 64LL * 1024 * 1024, ::Mu::Model::IdleTrimLevel::Balanced
        };
        auto memoryOnly = original;
        memoryOnly.memoryCacheBytes = 128LL * 1024 * 1024;
        QVERIFY(!::Mu::Generator::Config::renderingOutputChanged(original, memoryOnly));

        // Idle-trim changes affect resource usage, not pixels: no re-render,
        // but the worker still receives the new level live via setSettings.
        auto trimOnly = original;
        trimOnly.idleTrimAggressiveness = ::Mu::Model::IdleTrimLevel::Aggressive;
        QVERIFY(!::Mu::Generator::Config::renderingOutputChanged(original, trimOnly));
        QVERIFY(trimOnly != original);

        auto antialiasing = original;
        antialiasing.graphicsAntialiasing = 8;
        QVERIFY(::Mu::Generator::Config::renderingOutputChanged(original, antialiasing));
    }

    void propagatesOverprintSimulation_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("off") << false;
        QTest::newRow("on") << true;
    }

    void propagatesOverprintSimulation()
    {
        QFETCH(bool, enabled);
        QVERIFY(!::Mu::Model::DocumentSettings { }.overprintSimulation);
        QVERIFY(!::Mu::Generator::Config::RenderingSettings { }.overprintSimulation);
        const bool original = MuPDFNGSettings::overprintSimulation();
        MuPDFNGSettings::setOverprintSimulation(enabled);
        const auto settings = ::Mu::Generator::Config::readWorkerSettings();
        MuPDFNGSettings::setOverprintSimulation(original);
        QCOMPARE(settings.rendering.overprintSimulation, enabled);
        QCOMPARE(settings.documentSettings(0xFFFFFF).overprintSimulation, enabled);
        auto previous = settings.rendering;
        previous.overprintSimulation = !enabled;
        QVERIFY(::Mu::Generator::Config::renderingOutputChanged(previous, settings.rendering));
    }

    void clampsUnknownIdleTrimLevel()
    {
        ::Mu::Generator::Config::RenderingSettings rendering { 4, 6, 2, false, 64LL * 1024 * 1024, 99 };
        const auto settings = ::Mu::Generator::Config::documentSettingsFor(
            rendering, ::Mu::Generator::Config::EpubSettings { }, 0xFFFFFF);
        QCOMPARE(settings.idleTrimAggressiveness, ::Mu::Model::IdleTrimLevel::Balanced);
    }

    void clampedSettingsAlwaysValidate()
    {
        using ::Mu::Generator::Config::EpubSettings;
        using ::Mu::Generator::Config::RenderingSettings;

        // Extreme UI values must clamp into the shared worker bounds, which is
        // the single source of truth the validator enforces.
        const std::array<RenderingSettings, 4> renderings {
            RenderingSettings { -5, 99, 9, true, -1LL, ::Mu::Model::IdleTrimLevel::Off },
            RenderingSettings { 0, 0, 0, false, 0LL, -7 },
            RenderingSettings { 99, 99, 99, true, 1LL << 40, ::Mu::Model::IdleTrimLevel::Aggressive },
            RenderingSettings { 8, 8, 2, true, 256LL * 1024 * 1024, ::Mu::Model::IdleTrimLevel::Balanced },
        };
        const std::array<EpubSettings, 3> epubs {
            EpubSettings { -100, -100, -100, QStringLiteral("Ym9keXt9") },
            EpubSettings { 0, 0, 0, QString() },
            EpubSettings { 999, 999, 999, QStringLiteral("Ym9keXt9") },
        };

        for (const auto& rendering : renderings) {
            for (const auto& epub : epubs) {
                const auto settings = ::Mu::Generator::Config::documentSettingsFor(rendering, epub, 0);
                std::string_view reason;
                QVERIFY2(::Mu::Model::isValidDocumentSettings(settings, &reason), std::string(reason).c_str());
            }
        }
    }

    void buildsOcrConfiguration()
    {
        ::Mu::Generator::Config::OcrSettings settings;
        settings.language = QStringLiteral("eng");
        settings.dpi = 300;
        settings.force = true;
        settings.notify = true;
        settings.debounceMs = 100;
        const auto target = ::Mu::Generator::Config::ocrTargetFor(QStringLiteral("hash"), settings);
        const auto config = ::Mu::Generator::Config::ocrConfigFor(target, 12, 144, 144, settings);

        QCOMPARE(config.documentHash, QStringLiteral("hash"));
        QCOMPARE(config.language, QStringLiteral("eng"));
        QCOMPARE(config.pageCount, 12);
        QCOMPARE(config.dpi, 300);
        QVERIFY(config.force);
        QCOMPARE(config.debounceMs, 100);
    }

    void clampsOcrDebounceDelay()
    {
        const int originalDebounce = MuPDFNGSettings::ocrDebounceMs();
        MuPDFNGSettings::setOcrDebounceMs(5000);
        QCOMPARE(::Mu::Generator::Config::readOcrSettings().debounceMs, 2000);
        MuPDFNGSettings::setOcrDebounceMs(50);
        QCOMPARE(::Mu::Generator::Config::readOcrSettings().debounceMs, 100);
        MuPDFNGSettings::setOcrDebounceMs(100);
        QCOMPARE(::Mu::Generator::Config::readOcrSettings().debounceMs, 100);
        MuPDFNGSettings::setOcrDebounceMs(originalDebounce);
    }

    void disablesAutomaticOcrWhenNeverSelected()
    {
        const int originalTriggerMode = MuPDFNGSettings::ocrTriggerMode();
        MuPDFNGSettings::setOcrTriggerMode(MuPDFNGSettings::EnumOcrTriggerMode::Never);

        const auto settings = ::Mu::Generator::Config::readOcrSettings();
        MuPDFNGSettings::setOcrTriggerMode(originalTriggerMode);

        QVERIFY(!settings.force);
        QVERIFY(!settings.autoTrigger);
    }

    void disablesOcrTriggersWhenNoModelInstalled()
    {
        const QString originalLanguage = MuPDFNGSettings::ocrLanguage();
        const int originalTriggerMode = MuPDFNGSettings::ocrTriggerMode();
        MuPDFNGSettings::setOcrLanguage(QStringLiteral("-"));
        MuPDFNGSettings::setOcrTriggerMode(MuPDFNGSettings::EnumOcrTriggerMode::Always);

        const auto settings = ::Mu::Generator::Config::readOcrSettings();
        MuPDFNGSettings::setOcrLanguage(originalLanguage);
        MuPDFNGSettings::setOcrTriggerMode(originalTriggerMode);

        // An unset language follows the installed models, so either nothing
        // usable exists and OCR stays off, or a model was auto-picked (stored
        // stripped of its .traineddata suffix) and the trigger mode applies.
        if (settings.language == QStringLiteral("-")) {
            QVERIFY(!settings.force);
            QVERIFY(!settings.autoTrigger);
        } else {
            QVERIFY(settings.force);
            QVERIFY(!settings.language.isEmpty());
        }
    }

    void readsSignatureAppearanceOptions()
    {
        const int originalProfile = MuPDFNGSettings::signatureProfile();
        const bool originalUseUtc = MuPDFNGSettings::signatureUseUtc();
        const bool originalDrawBorder = MuPDFNGSettings::signatureDrawBorder();
        MuPDFNGSettings::setSignatureProfile(MuPDFNGSettings::EnumSignatureProfile::Simple);
        MuPDFNGSettings::setSignatureUseUtc(true);
        MuPDFNGSettings::setSignatureDrawBorder(true);

        const auto options = ::Mu::Generator::Config::readSignatureAppearance();
        MuPDFNGSettings::setSignatureProfile(originalProfile);
        MuPDFNGSettings::setSignatureUseUtc(originalUseUtc);
        MuPDFNGSettings::setSignatureDrawBorder(originalDrawBorder);

        QVERIFY(options.simple);
        QVERIFY(options.useUtc);
        QVERIFY(options.drawBorder);
    }

    void selectsInstalledOcrModel()
    {
        using ::Mu::Generator::Config::autoSelectOcrModel;

        QCOMPARE(autoSelectOcrModel({ }), QStringLiteral("-"));
        QCOMPARE(autoSelectOcrModel({ QStringLiteral("deu.traineddata") }), QStringLiteral("deu.traineddata"));
        QCOMPARE(autoSelectOcrModel({ QStringLiteral("eng.traineddata") }), QStringLiteral("eng.traineddata"));
        QCOMPARE(autoSelectOcrModel({ QStringLiteral("fra.traineddata"),
                                      QStringLiteral("eng.traineddata"),
                                      QStringLiteral("deu.traineddata") }),
                 QStringLiteral("eng.traineddata"));
        QCOMPARE(autoSelectOcrModel({ QStringLiteral("deu.traineddata"), QStringLiteral("fra.traineddata") }),
                 QStringLiteral("-"));
    }

    void listsInstalledOcrModels()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        for (const QString& file : { QStringLiteral("eng.traineddata"),
                                     QStringLiteral("deu.traineddata"),
                                     QStringLiteral("equ.traineddata"),
                                     QStringLiteral("readme.txt") }) {
            QFile entry(first.filePath(file));
            QVERIFY(entry.open(QIODevice::WriteOnly));
        }
        for (const QString& file : { QStringLiteral("deu.traineddata"),
                                     QStringLiteral("fra.traineddata"),
                                     QStringLiteral("osd.traineddata") }) {
            QFile entry(second.filePath(file));
            QVERIFY(entry.open(QIODevice::WriteOnly));
        }
        QVERIFY(QDir(first.path()).mkdir(QStringLiteral("sub")));
        QFile nested(first.filePath(QStringLiteral("sub/ita.traineddata")));
        QVERIFY(nested.open(QIODevice::WriteOnly));

        // Directories merge into one deduplicated, name-sorted list; support
        // files, notes, nested directories, and missing directories contribute
        // nothing.
        QCOMPARE(::Mu::Generator::Config::installedOcrModels({ first.path(), second.path() }),
                 QStringList({ QStringLiteral("deu.traineddata"),
                               QStringLiteral("eng.traineddata"),
                               QStringLiteral("fra.traineddata") }));
        QCOMPARE(::Mu::Generator::Config::installedOcrModels({ first.filePath(QStringLiteral("missing")) }),
                 QStringList());
    }

    void disablesOcrWhenLanguageIsNotAModelFilename()
    {
        const QString originalLanguage = MuPDFNGSettings::ocrLanguage();
        const int originalTriggerMode = MuPDFNGSettings::ocrTriggerMode();
        MuPDFNGSettings::setOcrLanguage(QStringLiteral("deu"));
        MuPDFNGSettings::setOcrTriggerMode(MuPDFNGSettings::EnumOcrTriggerMode::Always);

        const auto settings = ::Mu::Generator::Config::readOcrSettings();
        MuPDFNGSettings::setOcrLanguage(originalLanguage);
        MuPDFNGSettings::setOcrTriggerMode(originalTriggerMode);

        QCOMPARE(settings.language, QStringLiteral("-"));
        QVERIFY(!settings.force);
        QVERIFY(!settings.autoTrigger);
    }

    void usesDefaultCertificateDatabasePerPreference()
    {
        const bool original = MuPDFNGSettings::useDefaultCertDB();
        MuPDFNGSettings::setUseDefaultCertDB(true);
        QVERIFY(::Mu::Generator::Config::usesDefaultCertificateDatabase());
        MuPDFNGSettings::setUseDefaultCertDB(false);
        QVERIFY(!::Mu::Generator::Config::usesDefaultCertificateDatabase());
        MuPDFNGSettings::setUseDefaultCertDB(original);
    }

    void clampsPrintScaleMode()
    {
        const unsigned original = MuPDFNGSettings::printScaleMode();
        MuPDFNGSettings::setPrintScaleMode(0);
        QCOMPARE(::Mu::Generator::Config::readPrintScaleMode(), static_cast<std::uint32_t>(0));
        MuPDFNGSettings::setPrintScaleMode(2);
        QCOMPARE(::Mu::Generator::Config::readPrintScaleMode(), static_cast<std::uint32_t>(2));
        MuPDFNGSettings::setPrintScaleMode(99);
        QCOMPARE(::Mu::Generator::Config::readPrintScaleMode(), static_cast<std::uint32_t>(2));
        MuPDFNGSettings::setPrintScaleMode(original);
    }

    void normalizesTessdataDirectories()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        const QString firstPath = QDir::cleanPath(first.path());
        const QString secondPath = QDir::cleanPath(second.path());

        const QStringList input {
            QStringLiteral("relative"),
            firstPath + QStringLiteral("/"),
            secondPath + QStringLiteral("/tmp/../"),
            firstPath,
            QStringLiteral("~/tessdata"),
            QStringLiteral("/"),
            QStringLiteral("/nonexistent-mupdf-ng-tessdata"),
        };

        QCOMPARE(::Mu::Generator::Config::normalizeTessDataDirectories(input), QStringList({ firstPath, secondPath }));
    }

    void roundTripsCacheLastVacuum()
    {
        const QString original = MuPDFNGSettings::cacheLastVacuum();
        MuPDFNGSettings::setCacheLastVacuum(QStringLiteral("not-a-time"));
        QVERIFY(!::Mu::Generator::Config::readCacheLastVacuum().isValid());

        const QDateTime moment(QDate(2026, 1, 2), QTime(3, 4, 5), QTimeZone::UTC);
        ::Mu::Generator::Config::writeCacheLastVacuum(moment);
        QCOMPARE(::Mu::Generator::Config::readCacheLastVacuum(), moment);

        MuPDFNGSettings::setCacheLastVacuum(original);
    }
};

QTEST_MAIN(TestGeneratorConfig)

#include "test_config.moc"
