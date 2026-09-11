// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/settingswidget.hpp"

#include <KLocalizedString>
#include <QTimer>

#include "generator/config/certmanager/dialog_utils.hpp"
#include "generator/config/certmanager/manager_dialog.hpp"
#include "generator/config/settings.hpp"
#include "mupdfngsettings.h"
#include "plugin/crypto/nss.hpp"
#include "ui_settingswidget.h"

namespace Mu::Generator {

MuPDFSettingsWidget::MuPDFSettingsWidget(QWidget* parent)
    : QWidget(parent)
    , m_mupdfsw(new Ui_MuPDFSettingsWidgetBase)
{
    m_mupdfsw->setupUi(this);

    auto* gfxAA = m_mupdfsw->kcfg_GraphicsAntialiasingBits;
    gfxAA->clear();
    gfxAA->addItem(i18n("Disabled"), MuPDFSettings::EnumGraphicsAntialiasingBits::Disabled);
    gfxAA->addItem(i18n("Minimum"), MuPDFSettings::EnumGraphicsAntialiasingBits::Minimum);
    gfxAA->addItem(i18n("Low"), MuPDFSettings::EnumGraphicsAntialiasingBits::Low);
    gfxAA->addItem(i18n("Medium"), MuPDFSettings::EnumGraphicsAntialiasingBits::Medium);
    gfxAA->addItem(i18n("High"), MuPDFSettings::EnumGraphicsAntialiasingBits::High);

    auto* txtAA = m_mupdfsw->kcfg_TextAntialiasingBits;
    txtAA->clear();
    txtAA->addItem(i18n("Disabled"), MuPDFSettings::EnumTextAntialiasingBits::Disabled);
    txtAA->addItem(i18n("Minimum"), MuPDFSettings::EnumTextAntialiasingBits::Minimum);
    txtAA->addItem(i18n("Low"), MuPDFSettings::EnumTextAntialiasingBits::Low);
    txtAA->addItem(i18n("Medium"), MuPDFSettings::EnumTextAntialiasingBits::Medium);
    txtAA->addItem(i18n("High"), MuPDFSettings::EnumTextAntialiasingBits::High);

    auto* imgQ = m_mupdfsw->kcfg_ImageRenderingQuality;
    imgQ->clear();
    imgQ->addItem(i18n("Speed"), MuPDFSettings::EnumImageRenderingQuality::Speed);
    imgQ->addItem(i18n("Balanced"), MuPDFSettings::EnumImageRenderingQuality::Balanced);
    imgQ->addItem(i18n("Quality"), MuPDFSettings::EnumImageRenderingQuality::Quality);

    auto* memLimit = m_mupdfsw->kcfg_MemoryLimit;
    memLimit->clear();
    memLimit->addItem(i18n("32 MiB"), MuPDFSettings::EnumMemoryLimit::Size32MiB);
    memLimit->addItem(i18n("64 MiB"), MuPDFSettings::EnumMemoryLimit::Size64MiB);
    memLimit->addItem(i18n("128 MiB"), MuPDFSettings::EnumMemoryLimit::Size128MiB);
    memLimit->addItem(i18n("256 MiB"), MuPDFSettings::EnumMemoryLimit::Size256MiB);

    auto* idleTrim = m_mupdfsw->kcfg_IdleTrimLevel;
    idleTrim->clear();
    idleTrim->addItem(i18n("Off"), MuPDFSettings::EnumIdleTrimLevel::Off);
    idleTrim->addItem(i18n("Conservative"), MuPDFSettings::EnumIdleTrimLevel::Conservative);
    idleTrim->addItem(i18n("Balanced"), MuPDFSettings::EnumIdleTrimLevel::Balanced);
    idleTrim->addItem(i18n("Aggressive"), MuPDFSettings::EnumIdleTrimLevel::Aggressive);

    auto* sandboxEnforcement = m_mupdfsw->kcfg_SandboxEnforcement;
    sandboxEnforcement->clear();
    sandboxEnforcement->addItem(i18n("Relaxed"), MuPDFSettings::EnumSandboxEnforcement::Relaxed);
    sandboxEnforcement->addItem(i18n("Strict"), MuPDFSettings::EnumSandboxEnforcement::Strict);

    auto* epubPageSize = m_mupdfsw->kcfg_EpubPageSize;
    epubPageSize->clear();
    epubPageSize->addItem(i18n("A5 (148 × 210 mm)"), MuPDFSettings::EnumEpubPageSize::A5);
    epubPageSize->addItem(i18n("6×9 (152 × 229 mm)"), MuPDFSettings::EnumEpubPageSize::SixByNine);
    epubPageSize->addItem(i18n("B5 (176 × 250 mm)"), MuPDFSettings::EnumEpubPageSize::B5);
    epubPageSize->addItem(i18n("Letter (216 × 279 mm)"), MuPDFSettings::EnumEpubPageSize::Letter);

    auto* epubFontFamily = m_mupdfsw->kcfg_EpubFontFamily;
    epubFontFamily->clear();
    epubFontFamily->addItem(i18n("Default"), MuPDFSettings::EnumEpubFontFamily::Default);
    epubFontFamily->addItem(i18n("Serif"), MuPDFSettings::EnumEpubFontFamily::Serif);
    epubFontFamily->addItem(i18n("Sans-serif"), MuPDFSettings::EnumEpubFontFamily::SansSerif);
    epubFontFamily->addItem(i18n("Monospace"), MuPDFSettings::EnumEpubFontFamily::Monospace);

    m_mupdfsw->kcfg_EpubCustomCss->setVisible(false);
    updateCustomCssButtonText();
    connect(m_mupdfsw->customCssButton, &QPushButton::toggled, m_mupdfsw->kcfg_EpubCustomCss, &QWidget::setVisible);
    connect(m_mupdfsw->kcfg_EpubCustomCss,
            &CssEditor::encodedTextChanged,
            this,
            &MuPDFSettingsWidget::updateCustomCssButtonText);

    auto* ocrLang = m_mupdfsw->kcfg_OcrLanguage;
    ocrLang->clear();
    ocrLang->setEditable(false);
    ocrLang->setProperty("kcfg_property", QByteArrayLiteral("currentText"));

    const QStringList models = Config::installedOcrModels();

    // Keep the "-" no-OCR sentinel selectable whenever models exist; the
    // stored default is "-", so without this entry the combo would display
    // the first model and persist it on dialog accept, silently enabling OCR.
    ocrLang->addItem(QStringLiteral("-"), QStringLiteral("-"));
    for (const QString& file : models)
        ocrLang->addItem(file, file);

    // Preselect an installed model when nothing usable is stored yet. KConfigXT
    // applies the stored value during dialog setup after this constructor, so
    // the selection is deferred to the event loop to survive that load. This
    // only changes the display: Cancel still writes nothing, Accept persists
    // the visible choice through the existing currentText binding.
    QTimer::singleShot(0, this, [this, models] {
        auto* ocrLang = m_mupdfsw->kcfg_OcrLanguage;
        const QString stored = MuPDFSettings::ocrLanguage();
        if (!stored.isEmpty() && stored != QStringLiteral("-") && ocrLang->findData(stored) >= 0)
            return;
        const int index = ocrLang->findData(Config::autoSelectOcrModel(models));
        if (index >= 0)
            ocrLang->setCurrentIndex(index);
    });
    auto* ocrQuality = m_mupdfsw->kcfg_OcrQuality;
    ocrQuality->clear();
    ocrQuality->addItem(i18n("Speed (150dpi)"), MuPDFSettings::EnumOcrQuality::Speed);
    ocrQuality->addItem(i18n("Balanced (225dpi)"), MuPDFSettings::EnumOcrQuality::Balanced);
    ocrQuality->addItem(i18n("Accuracy (300dpi)"), MuPDFSettings::EnumOcrQuality::Accuracy);

    auto* triggerMode = m_mupdfsw->kcfg_OcrTriggerMode;
    triggerMode->clear();
    triggerMode->addItem(i18n("0 (Never)"), MuPDFSettings::EnumOcrTriggerMode::Never);
    triggerMode->addItem(i18n("5 characters"), MuPDFSettings::EnumOcrTriggerMode::Five);
    triggerMode->addItem(i18n("20 characters"), MuPDFSettings::EnumOcrTriggerMode::Twenty);
    triggerMode->addItem(i18n("∞ (Always)"), MuPDFSettings::EnumOcrTriggerMode::Always);
    const QString ocrTriggerToolTip =
        i18n("Automatically run OCR when the page contains fewer extracted characters than the selected threshold.\n"
             "Choose \"Never\" to disable automatic OCR or \"Always\" to run OCR on every page.");
    triggerMode->setToolTip(ocrTriggerToolTip);
    m_mupdfsw->labelOcrTriggerMode->setToolTip(ocrTriggerToolTip);

    m_mupdfsw->defaultLabel->setText(Plugin::Crypto::defaultSystemNssDbPath());

    connect(
        m_mupdfsw->customRadioButton, &QRadioButton::toggled, m_mupdfsw->kcfg_dBCertificatePath, &QWidget::setEnabled);
    if (MuPDFSettings::useDefaultCertDB()) {
        m_mupdfsw->kcfg_UseDefaultCertDB->setChecked(true);
    } else {
        m_mupdfsw->customRadioButton->setChecked(true);
    }
    updateManageCertificatesButton();

    connect(m_mupdfsw->manageCertificatesButton, &QPushButton::clicked, this, [this] {
        const QString databasePath = Plugin::Crypto::activeNssDatabasePath();
        CertificateManagerDialog dialog(databasePath, this);
        dialog.exec();
    });
}

void MuPDFSettingsWidget::updateCustomCssButtonText()
{
    const bool hasCustomCss = !m_mupdfsw->kcfg_EpubCustomCss->encodedText().isEmpty();
    m_mupdfsw->customCssButton->setText(hasCustomCss ? i18n("Custom CSS (configured)") : i18n("Custom CSS"));
}

void MuPDFSettingsWidget::updateManageCertificatesButton()
{
    const QString databasePath = Plugin::Crypto::activeNssDatabasePath();
    const QString databaseLabel = databasePath.isEmpty() ? i18n("NSS database unavailable")
                                                         : CertificateManager::displayDatabasePath(databasePath);
    m_mupdfsw->manageCertificatesButton->setText(i18n("Manage Certificates - %1", databaseLabel));
}

MuPDFSettingsWidget::~MuPDFSettingsWidget()
{
    delete m_mupdfsw;
}

} // namespace Mu::Generator
