// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/mupdfsettingswidget.hpp"

#include <KLocalizedString>
#include <QDir>

#include "generator/config/certmanager/dialog_utils.hpp"
#include "generator/config/certmanager/manager_dialog.hpp"
#include "mupdfsettings.h"
#include "plugin/crypto/nss.hpp"
#include "ui_mupdfsettingswidget.h"

namespace Mu::Generator {

MuPDFSettingsWidget::MuPDFSettingsWidget(QWidget* parent, const Model::SandboxStatus& sandboxStatus)
    : QWidget(parent)
    , m_mupdfsw(new Ui_MuPDFSettingsWidgetBase)
{
    m_mupdfsw->setupUi(this);
    m_mupdfsw->securityDetailsWidget->setVisible(false);
    m_mupdfsw->securityDetailsButton->setArrowType(Qt::RightArrow);
    connect(m_mupdfsw->securityDetailsButton, &QToolButton::toggled, this, [this](bool checked) {
        m_mupdfsw->securityDetailsWidget->setVisible(checked);
        m_mupdfsw->securityDetailsButton->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    });
    setSandboxStatus(sandboxStatus);

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
    imgQ->addItem(i18n("Balance"), MuPDFSettings::EnumImageRenderingQuality::Balance);
    imgQ->addItem(i18n("Quality"), MuPDFSettings::EnumImageRenderingQuality::Quality);
    imgQ->addItem(i18n("Speed"), MuPDFSettings::EnumImageRenderingQuality::Speed);

    auto* memLimit = m_mupdfsw->kcfg_MemoryLimit;
    memLimit->clear();
    memLimit->addItem(i18n("32 MiB"), MuPDFSettings::EnumMemoryLimit::Size32MiB);
    memLimit->addItem(i18n("64 MiB"), MuPDFSettings::EnumMemoryLimit::Size64MiB);
    memLimit->addItem(i18n("128 MiB"), MuPDFSettings::EnumMemoryLimit::Size128MiB);
    memLimit->addItem(i18n("256 MiB"), MuPDFSettings::EnumMemoryLimit::Size256MiB);

    auto* epubPageSize = m_mupdfsw->kcfg_EpubPageSize;
    epubPageSize->clear();
    epubPageSize->addItem(i18n("B5 (176 × 250 mm)"), MuPDFSettings::EnumEpubPageSize::B5);
    epubPageSize->addItem(i18n("A5 (148 × 210 mm)"), MuPDFSettings::EnumEpubPageSize::A5);
    epubPageSize->addItem(i18n("6×9 (152 × 229 mm)"), MuPDFSettings::EnumEpubPageSize::SixByNine);
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

#ifndef MUPDF_HAS_OCR
    m_mupdfsw->ocrGroupBox->hide();
#else
    auto* ocrLang = m_mupdfsw->kcfg_OcrLanguage;
    ocrLang->clear();
    ocrLang->setEditable(false);
    ocrLang->setProperty("kcfg_property", QByteArrayLiteral("currentText"));

#ifndef TESSDATA_DIR
#define TESSDATA_DIR "/usr/share/tessdata"
#endif

    const QString tessDir = QStringLiteral(TESSDATA_DIR);
    QDir dir(tessDir);
    const QStringList files = dir.entryList({ QStringLiteral("*.traineddata") }, QDir::Files, QDir::Name);

    if (!files.isEmpty()) {
        for (const QString& file : files) {
            if (file == QStringLiteral("equ.traineddata") || file == QStringLiteral("osd.traineddata")) {
                continue;
            }
            ocrLang->addItem(file, file);
        }
    } else {
        // Fallback default list if directory is missing or empty
        const QStringList defaults = { QStringLiteral("eng.traineddata") };
        for (const QString& defItem : defaults) {
            ocrLang->addItem(defItem, defItem);
        }
    }

    auto* ocrQuality = m_mupdfsw->kcfg_OcrQuality;
    ocrQuality->clear();
    ocrQuality->addItem(i18n("Speed (150dpi)"), MuPDFSettings::EnumOcrQuality::Speed);
    ocrQuality->addItem(i18n("Balance (225dpi)"), MuPDFSettings::EnumOcrQuality::Balance);
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
#endif // MUPDF_HAS_OCR

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

void MuPDFSettingsWidget::setSandboxStatus(const Model::SandboxStatus& status)
{
    const bool active = status.linuxNamespace || status.seccomp || status.landlock;
    if (active) {
        m_mupdfsw->labelOverallSecurityStatus->setText(
            QStringLiteral("<font color=\"#27ae60\"><b>%1</b></font>").arg(i18n("Active")));
    } else {
        m_mupdfsw->labelOverallSecurityStatus->setText(
            QStringLiteral("<font color=\"#e74c3c\"><b>%1</b></font>").arg(i18n("Inactive")));
    }

    auto formatComponent = [](bool isComponentActive, const QString& activeText = { }) {
        if (isComponentActive) {
            const QString label = activeText.isEmpty() ? i18n("Active") : activeText;
            return QStringLiteral("<font color=\"#27ae60\">%1</font>").arg(label);
        }
        return QStringLiteral("<font color=\"#e74c3c\">%1</font>").arg(i18n("Inactive"));
    };

    m_mupdfsw->labelNamespaceStatus->setText(formatComponent(status.linuxNamespace));
    m_mupdfsw->labelSeccompStatus->setText(formatComponent(status.seccomp));
    const QString landlockText = status.landlockAbi > 0 ? i18n("Active (ABI v%1)", status.landlockAbi) : i18n("Active");
    m_mupdfsw->labelLandlockStatus->setText(formatComponent(status.landlock, landlockText));
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
