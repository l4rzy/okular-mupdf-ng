// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KLocalizedString>
#include <QTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>
#include <array>
#include <cstdint>
#include <string_view>

#include "generator/config/certmanager/dialog_utils.hpp"
#include "generator/config/certmanager/manager_dialog.hpp"
#include "generator/config/certmanager/self_signed_dialog.hpp"
#include "generator/config/document_settings.hpp"
#include "generator/config/settings.hpp"
#include "mupdfngsettings.h"
#include "plugin/crypto/nss.hpp"
#include "shared/model/validation.hpp"

using Mu::Generator::Config::SignatureEmblem;

class TestGeneratorConfig : public QObject {
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
        KLocalizedString::setApplicationDomain("okular_mupdfng");
    }

    void documentOverrides_data()
    {
        QTest::addColumn<bool>("theme");
        QTest::addColumn<bool>("layout");
        QTest::addColumn<bool>("ocr");
        QTest::newRow("inherit") << false << false << false;
        QTest::newRow("theme") << true << false << false;
        QTest::newRow("layout") << false << true << false;
        QTest::newRow("ocr") << false << false << true;
        QTest::newRow("all") << true << true << true;
    }

    void documentOverrides()
    {
        namespace Config = Mu::Generator::Config;
        QFETCH(bool, theme);
        QFETCH(bool, layout);
        QFETCH(bool, ocr);
        Config::DocumentSettings settings;
        if (theme)
            settings.theme = Mu::Model::DocumentTheme::Dusk;
        if (layout)
            settings.layout =
                Config::EpubSettings { 16, 2, 0, QString::fromLatin1(QByteArray("body { color: red; }").toBase64()) };
        if (ocr) {
            settings.ocr = Config::OcrSettings();
            settings.ocr->language = QStringLiteral("vie");
            settings.ocr->dpi = 300;
            settings.ocr->notify = false;
            Config::setOcrTriggerMode(*settings.ocr, 3);
        }
        const auto key = QStringLiteral("test-document");
        QVERIFY(Config::writeDocumentSettings(key, settings));
        QVERIFY(Config::readDocumentSettings(key) == settings);
        QVERIFY(Config::readDocumentSettings(QStringLiteral("other-document")) == Config::DocumentSettings());
        QVERIFY(Config::writeDocumentSettings(key, { }));
        QVERIFY(Config::readDocumentSettings(key) == Config::DocumentSettings());
        KConfig config(QStringLiteral("okular-mupdf-ng/documentsrc"), KConfig::SimpleConfig);
        QVERIFY(!config.hasGroup(key));
    }

    void ocrModeRoundTrip_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("off") << 0;
        QTest::newRow("automatic") << 1;
        QTest::newRow("sparse") << 2;
        QTest::newRow("always") << 3;
    }

    void ocrModeRoundTrip()
    {
        QFETCH(int, mode);
        Mu::Generator::Config::OcrSettings settings;
        Mu::Generator::Config::setOcrTriggerMode(settings, mode);
        QCOMPARE(Mu::Generator::Config::ocrTriggerModeFor(settings), mode);
        QCOMPARE(settings.force, mode == 3);
        QCOMPARE(settings.autoTrigger, mode != 0);
        QCOMPARE(settings.triggerThreshold, mode == 1 ? 5U : 20U);
    }

    void documentIdentity()
    {
        namespace Config = Mu::Generator::Config;
        QFile file(m_root.filePath("identity.epub"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("first");
        file.close();
        const auto key = Config::documentSettingsKey(file.fileName());
        QVERIFY(!key.isEmpty());
        QCOMPARE(Config::documentSettingsKey(file.fileName()), key);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("replacement");
        file.close();
        QCOMPARE(Config::documentSettingsKey(file.fileName()), key);
        QFile other(m_root.filePath("other.epub"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write("replacement");
        other.close();
        QVERIFY(Config::documentSettingsKey(other.fileName()) != key);
        QVERIFY(Config::documentSettingsKey(QStringLiteral("/missing/document.epub")).isEmpty());
        const QByteArray data(200000, 'x');
        QCOMPARE(Config::documentSettingsKey(data), Config::documentSettingsKey(data));
        QByteArray changed = data;
        changed.back() = 'y';
        QVERIFY(Config::documentSettingsKey(data) != Config::documentSettingsKey(changed));
        changed = data;
        changed[100000] = 'y';
        QVERIFY(Config::documentSettingsKey(data) != Config::documentSettingsKey(changed));
    }

    void defaultsLinkPreviewToOff()
    {
        QCOMPARE(MuPDFNGSettings::linkPreviewMode(), MuPDFNGSettings::EnumLinkPreviewMode::Off);
        QCOMPARE(::Mu::Generator::Config::readLinkPreviewMode(), ::Mu::Generator::LinkPreviewMode::Off);
    }

    void readsLinkPreviewMode_data()
    {
        using Mode = ::Mu::Generator::LinkPreviewMode;
        QTest::addColumn<int>("stored");
        QTest::addColumn<Mode>("expected");
        QTest::newRow("off") << int(MuPDFNGSettings::EnumLinkPreviewMode::Off) << Mode::Off;
        QTest::newRow("auto") << int(MuPDFNGSettings::EnumLinkPreviewMode::AutoSize) << Mode::AdaptiveSize;
        QTest::newRow("small") << int(MuPDFNGSettings::EnumLinkPreviewMode::Small) << Mode::Small;
        QTest::newRow("large") << int(MuPDFNGSettings::EnumLinkPreviewMode::Large) << Mode::Large;
    }

    void readsLinkPreviewMode()
    {
        QFETCH(int, stored);
        QFETCH(::Mu::Generator::LinkPreviewMode, expected);
        const int original = MuPDFNGSettings::linkPreviewMode();
        MuPDFNGSettings::setLinkPreviewMode(stored);
        MuPDFNGSettings::self()->save();
        ::Mu::Generator::Config::reloadSettings();
        const auto mode = ::Mu::Generator::Config::readLinkPreviewMode();
        MuPDFNGSettings::setLinkPreviewMode(original);
        MuPDFNGSettings::self()->save();
        QCOMPARE(mode, expected);
    }

    void certificateManagerSeparatesSigningAndPeople()
    {
        namespace Database = ::Mu::Plugin::Crypto::CertificateDatabase;
        const auto path = m_root.filePath("certmanager-db");
        QVERIFY(QDir().mkpath(path));
        QCOMPARE(::Mu::Plugin::Crypto::initializeNss(path), ::Mu::Plugin::Crypto::NssRuntimeMode::ReadWrite);
        Database::SelfSignedCertificateOptions options;
        options.nickname = QStringLiteral("synthetic-ui-signer");
        options.commonName = QStringLiteral("Synthetic UI Signer");
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addDays(1);
        options.signingKey = Database::SigningKey::EcdsaP256;
        QString error;
        QVERIFY2(Database::createSelfSignedCertificate(path, options, &error), qPrintable(error));
        auto records = Database::listCertificates(path, &error);
        QCOMPARE(records.size(), 1);
        const auto& certificate = records.front().certificate;
        const QByteArray der(reinterpret_cast<const char*>(certificate.der.data()),
                             static_cast<qsizetype>(certificate.der.size()));
        ::Mu::Generator::CertificateManagerDialog dialog(path);
        auto* tabs = dialog.findChild<QTabWidget*>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(0), QStringLiteral("My Certificates"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("People"));
        auto* signing = tabs->widget(0)->findChild<QTableWidget*>();
        auto* people = tabs->widget(1)->findChild<QTableWidget*>();
        QVERIFY(signing);
        QVERIFY(people);
        QCOMPARE(signing->rowCount(), 1);
        tabs->setCurrentIndex(1);
        QCOMPARE(people->rowCount(), 0);
        QVERIFY2(Database::importTrustedSigner(path, der, options.nickname, &error), qPrintable(error));
        tabs->setCurrentIndex(0);
        tabs->setCurrentIndex(1);
        QCOMPARE(people->rowCount(), 1);
        people->selectRow(0);
        QPushButton* remove = nullptr;
        for (auto* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Remove Trust"))
                remove = button;
        }
        QVERIFY(remove);
        QVERIFY(remove->isEnabled());
        tabs->setCurrentIndex(0);
        tabs->setCurrentIndex(1);
        people->selectRow(0);
        QVERIFY(remove->isEnabled());
        QTimer::singleShot(0, &dialog, [&dialog] {
            for (auto* message : dialog.findChildren<QMessageBox*>())
                message->button(QMessageBox::Yes)->click();
        });
        remove->click();
        QCOMPARE(people->rowCount(), 0);
        QCOMPARE(Database::listTrustedSigners(path).size(), 0);
        tabs->setCurrentIndex(0);
        QCOMPARE(signing->rowCount(), 1);
        QCOMPARE(::Mu::Plugin::Crypto::createDetachedCmsFromDigest(options.nickname, { }, { }).result,
                 ::Mu::Model::SigningResult::Success);
        QVERIFY2(Database::importTrustedSigner(path, der, options.nickname, &error), qPrintable(error));
        signing->selectRow(0);
        QTimer::singleShot(0, &dialog, [&dialog] {
            for (auto* message : dialog.findChildren<QMessageBox*>())
                message->button(QMessageBox::Yes)->click();
        });
        remove->click();
        QCOMPARE(signing->rowCount(), 0);
        tabs->setCurrentIndex(1);
        QCOMPARE(people->rowCount(), 0);
        QVERIFY(Database::listCertificates(path).isEmpty());
        QVERIFY(Database::listTrustedSigners(path).isEmpty());
    }

    void selectsSelfSignedTrust_data()
    {
        QTest::addColumn<bool>("trusted");
        QTest::newRow("signing-only") << false;
        QTest::newRow("trusted") << true;
    }

    void selectsSelfSignedTrust()
    {
        QFETCH(bool, trusted);
        Mu::Generator::SelfSignedCertificateDialog dialog(QStringLiteral("test-db"));
        auto* checkbox = dialog.findChild<QCheckBox*>(QStringLiteral("trustForVerification"));
        QVERIFY(checkbox);
        QVERIFY(!checkbox->isChecked());
        QVERIFY(!dialog.certificateOptions().trustForVerification);
        checkbox->setChecked(trusted);
        QCOMPARE(dialog.certificateOptions().trustForVerification, trusted);
    }

    void defaultsRenderCancellationToDisabled() { QVERIFY(!::Mu::Generator::Config::readRenderCancellationEnabled()); }

    void readsRenderCancellation_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("enabled") << true;
        QTest::newRow("disabled") << false;
    }

    void readsRenderCancellation()
    {
        QFETCH(bool, enabled);
        const bool original = MuPDFNGSettings::cancelObsoleteRenders();
        MuPDFNGSettings::setCancelObsoleteRenders(enabled);
        MuPDFNGSettings::self()->save();
        ::Mu::Generator::Config::reloadSettings();
        const bool actual = ::Mu::Generator::Config::readRenderCancellationEnabled();
        MuPDFNGSettings::setCancelObsoleteRenders(original);
        MuPDFNGSettings::self()->save();
        QCOMPARE(actual, enabled);
    }

    void defaultsHeuristicSynopsisToEnabled() { QVERIFY(::Mu::Generator::Config::readHeuristicSynopsisEnabled()); }

    void readsHeuristicSynopsis_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("enabled") << true;
        QTest::newRow("disabled") << false;
    }

    void readsHeuristicSynopsis()
    {
        QFETCH(bool, enabled);
        const bool original = MuPDFNGSettings::heuristicSynopsisEnabled();
        MuPDFNGSettings::setHeuristicSynopsisEnabled(enabled);
        MuPDFNGSettings::self()->save();
        ::Mu::Generator::Config::reloadSettings();
        const bool actual = ::Mu::Generator::Config::readHeuristicSynopsisEnabled();
        MuPDFNGSettings::setHeuristicSynopsisEnabled(original);
        MuPDFNGSettings::self()->save();
        QCOMPARE(actual, enabled);
    }

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

    void defaultsSignatureEmblemToOkular()
    {
        QCOMPARE(MuPDFNGSettings::signatureEmblem(), MuPDFNGSettings::EnumSignatureEmblem::Okular);
        QCOMPARE(::Mu::Generator::Config::readSignatureAppearance().emblem, SignatureEmblem::Okular);
    }

    void readsSignatureAppearanceOptions_data()
    {
        QTest::addColumn<int>("emblem");
        QTest::newRow("none") << int(MuPDFNGSettings::EnumSignatureEmblem::None);
        QTest::newRow("mupdf") << int(MuPDFNGSettings::EnumSignatureEmblem::MuPDF);
        QTest::newRow("okular") << int(MuPDFNGSettings::EnumSignatureEmblem::Okular);
    }

    void readsSignatureAppearanceOptions()
    {
        QFETCH(int, emblem);
        const int originalProfile = MuPDFNGSettings::signatureProfile();
        const int originalEmblem = MuPDFNGSettings::signatureEmblem();
        const bool originalUseUtc = MuPDFNGSettings::signatureUseUtc();
        const bool originalDrawBorder = MuPDFNGSettings::signatureDrawBorder();
        MuPDFNGSettings::setSignatureProfile(MuPDFNGSettings::EnumSignatureProfile::Simple);
        MuPDFNGSettings::setSignatureUseUtc(true);
        MuPDFNGSettings::setSignatureDrawBorder(true);
        MuPDFNGSettings::setSignatureEmblem(emblem);
        MuPDFNGSettings::self()->save();
        ::Mu::Generator::Config::reloadSettings();

        const auto options = ::Mu::Generator::Config::readSignatureAppearance();
        MuPDFNGSettings::setSignatureProfile(originalProfile);
        MuPDFNGSettings::setSignatureUseUtc(originalUseUtc);
        MuPDFNGSettings::setSignatureDrawBorder(originalDrawBorder);
        MuPDFNGSettings::setSignatureEmblem(originalEmblem);
        MuPDFNGSettings::self()->save();

        QVERIFY(options.simple);
        QVERIFY(options.useUtc);
        QVERIFY(options.drawBorder);
        QCOMPARE(options.emblem, static_cast<SignatureEmblem>(emblem));
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
