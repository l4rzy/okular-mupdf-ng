// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/settingswidget.hpp"

#include <KLineEdit>
#include <KLocalizedString>
#include <QApplication>
#include <QLayout>
#include <QTimer>
#include <QWindow>

#include <initializer_list>

#include "generator/config/certmanager/dialog_utils.hpp"
#include "generator/config/certmanager/manager_dialog.hpp"
#include "generator/config/settings.hpp"
#include "generator/config/signature_preview.hpp"
#include "generator/config/tooltip.hpp"
#include "mupdfngsettings.h"
#include "plugin/crypto/nss.hpp"
#include "ui_settingswidget.h"

namespace Mu::Generator {

MuPDFNGSettingsWidget::MuPDFNGSettingsWidget(QWidget* parent)
    : QWidget(parent)
    , m_mupdfsw(new Ui_MuPDFNGSettingsWidgetBase)
{
    m_mupdfsw->setupUi(this);
    setupToolTips();

#ifndef MU_WORKER_ENABLE_FORM_JAVASCRIPT
    m_mupdfsw->kcfg_PdfFormJavaScriptEnabled->setEnabled(false);
    m_mupdfsw->kcfg_PdfFormJavaScriptEnabled->setToolTip(
        Config::wrapToolTip(i18n("PDF form JavaScript support was disabled when this application was built.")));
#endif

    auto* gfxAA = m_mupdfsw->kcfg_GraphicsAntialiasingBits;
    gfxAA->clear();
    gfxAA->addItem(i18n("Disabled"), MuPDFNGSettings::EnumGraphicsAntialiasingBits::Disabled);
    gfxAA->addItem(i18n("Minimum"), MuPDFNGSettings::EnumGraphicsAntialiasingBits::Minimum);
    gfxAA->addItem(i18n("Low"), MuPDFNGSettings::EnumGraphicsAntialiasingBits::Low);
    gfxAA->addItem(i18n("Medium"), MuPDFNGSettings::EnumGraphicsAntialiasingBits::Medium);
    gfxAA->addItem(i18n("High"), MuPDFNGSettings::EnumGraphicsAntialiasingBits::High);

    auto* txtAA = m_mupdfsw->kcfg_TextAntialiasingBits;
    txtAA->clear();
    txtAA->addItem(i18n("Disabled"), MuPDFNGSettings::EnumTextAntialiasingBits::Disabled);
    txtAA->addItem(i18n("Minimum"), MuPDFNGSettings::EnumTextAntialiasingBits::Minimum);
    txtAA->addItem(i18n("Low"), MuPDFNGSettings::EnumTextAntialiasingBits::Low);
    txtAA->addItem(i18n("Medium"), MuPDFNGSettings::EnumTextAntialiasingBits::Medium);
    txtAA->addItem(i18n("High"), MuPDFNGSettings::EnumTextAntialiasingBits::High);

    auto* imgQ = m_mupdfsw->kcfg_ImageRenderingQuality;
    imgQ->clear();
    imgQ->addItem(i18n("Speed"), MuPDFNGSettings::EnumImageRenderingQuality::Speed);
    imgQ->addItem(i18n("Balanced"), MuPDFNGSettings::EnumImageRenderingQuality::Balanced);
    imgQ->addItem(i18n("Quality"), MuPDFNGSettings::EnumImageRenderingQuality::Quality);

    auto* memLimit = m_mupdfsw->kcfg_MemoryLimit;
    memLimit->clear();
    memLimit->addItem(i18n("32 MiB"), MuPDFNGSettings::EnumMemoryLimit::Size32MiB);
    memLimit->addItem(i18n("64 MiB"), MuPDFNGSettings::EnumMemoryLimit::Size64MiB);
    memLimit->addItem(i18n("128 MiB"), MuPDFNGSettings::EnumMemoryLimit::Size128MiB);
    memLimit->addItem(i18n("256 MiB"), MuPDFNGSettings::EnumMemoryLimit::Size256MiB);

    auto* idleTrim = m_mupdfsw->kcfg_IdleTrimLevel;
    idleTrim->clear();
    idleTrim->addItem(i18n("Off"), MuPDFNGSettings::EnumIdleTrimLevel::Off);
    idleTrim->addItem(i18n("Conservative"), MuPDFNGSettings::EnumIdleTrimLevel::Conservative);
    idleTrim->addItem(i18n("Balanced"), MuPDFNGSettings::EnumIdleTrimLevel::Balanced);
    idleTrim->addItem(i18n("Aggressive"), MuPDFNGSettings::EnumIdleTrimLevel::Aggressive);

    auto* sandboxEnforcement = m_mupdfsw->kcfg_SandboxEnforcement;
    sandboxEnforcement->clear();
    sandboxEnforcement->addItem(i18n("Relaxed"), MuPDFNGSettings::EnumSandboxEnforcement::Relaxed);
    sandboxEnforcement->addItem(i18n("Strict"), MuPDFNGSettings::EnumSandboxEnforcement::Strict);

    auto* epubPageSize = m_mupdfsw->kcfg_EpubPageSize;
    epubPageSize->clear();
    epubPageSize->addItem(i18n("A5 (148 × 210 mm)"), MuPDFNGSettings::EnumEpubPageSize::A5);
    epubPageSize->addItem(i18n("6×9 (152 × 229 mm)"), MuPDFNGSettings::EnumEpubPageSize::SixByNine);
    epubPageSize->addItem(i18n("B5 (176 × 250 mm)"), MuPDFNGSettings::EnumEpubPageSize::B5);
    epubPageSize->addItem(i18n("Letter (216 × 279 mm)"), MuPDFNGSettings::EnumEpubPageSize::Letter);

    auto* epubFontFamily = m_mupdfsw->kcfg_EpubFontFamily;
    epubFontFamily->clear();
    epubFontFamily->addItem(i18n("Default"), MuPDFNGSettings::EnumEpubFontFamily::Default);
    epubFontFamily->addItem(i18n("Serif"), MuPDFNGSettings::EnumEpubFontFamily::Serif);
    epubFontFamily->addItem(i18n("Sans-serif"), MuPDFNGSettings::EnumEpubFontFamily::SansSerif);
    epubFontFamily->addItem(i18n("Monospace"), MuPDFNGSettings::EnumEpubFontFamily::Monospace);

    m_mupdfsw->kcfg_EpubCustomCss->setVisible(false);
    updateCustomCssButtonText();
    // Settle the nested layouts before repainting so queued layout requests
    // cannot expose intermediate positions when the editor is shown or hidden.
    connect(m_mupdfsw->customCssButton, &QPushButton::toggled, this, [this](bool checked) {
        setUpdatesEnabled(false);
        m_mupdfsw->kcfg_EpubCustomCss->setVisible(checked);
        for (QWidget* container = m_mupdfsw->kcfg_EpubCustomCss->parentWidget(); container;
             container = container->parentWidget()) {
            if (QLayout* layout = container->layout())
                layout->activate();
        }
        setUpdatesEnabled(true);
    });
    connect(m_mupdfsw->kcfg_EpubCustomCss,
            &CssEditor::encodedTextChanged,
            this,
            &MuPDFNGSettingsWidget::updateCustomCssButtonText);

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
        const QString stored = MuPDFNGSettings::ocrLanguage();
        if (!stored.isEmpty() && stored != QStringLiteral("-") && ocrLang->findData(stored) >= 0)
            return;
        const int index = ocrLang->findData(Config::autoSelectOcrModel(models));
        if (index >= 0)
            ocrLang->setCurrentIndex(index);
    });
    auto* ocrQuality = m_mupdfsw->kcfg_OcrQuality;
    ocrQuality->clear();
    ocrQuality->addItem(i18n("Speed (150dpi)"), MuPDFNGSettings::EnumOcrQuality::Speed);
    ocrQuality->addItem(i18n("Balanced (225dpi)"), MuPDFNGSettings::EnumOcrQuality::Balanced);
    ocrQuality->addItem(i18n("Accuracy (300dpi)"), MuPDFNGSettings::EnumOcrQuality::Accuracy);

    auto* triggerMode = m_mupdfsw->kcfg_OcrTriggerMode;
    triggerMode->clear();
    triggerMode->addItem(i18n("0 (Never)"), MuPDFNGSettings::EnumOcrTriggerMode::Never);
    triggerMode->addItem(i18n("5 characters"), MuPDFNGSettings::EnumOcrTriggerMode::Five);
    triggerMode->addItem(i18n("20 characters"), MuPDFNGSettings::EnumOcrTriggerMode::Twenty);
    triggerMode->addItem(i18n("∞ (Always)"), MuPDFNGSettings::EnumOcrTriggerMode::Always);

    m_mupdfsw->defaultLabel->setText(Plugin::Crypto::defaultSystemNssDbPath());

    auto* signatureProfile = m_mupdfsw->kcfg_SignatureProfile;
    signatureProfile->clear();
    signatureProfile->addItem(i18n("Complete"), MuPDFNGSettings::EnumSignatureProfile::Complete);
    signatureProfile->addItem(i18n("Simple"), MuPDFNGSettings::EnumSignatureProfile::Simple);
    auto* signatureEmblem = m_mupdfsw->kcfg_SignatureEmblem;
    signatureEmblem->clear();
    signatureEmblem->addItem(i18n("None"), MuPDFNGSettings::EnumSignatureEmblem::None);
    signatureEmblem->addItem(i18n("MuPDF"), MuPDFNGSettings::EnumSignatureEmblem::MuPDF);
    signatureEmblem->addItem(i18n("Okular"), MuPDFNGSettings::EnumSignatureEmblem::Okular);
    connect(signatureEmblem,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MuPDFNGSettingsWidget::updateSignaturePreview);
    connect(signatureProfile,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MuPDFNGSettingsWidget::updateSignaturePreview);
    connect(m_mupdfsw->kcfg_SignatureUseUtc, &QCheckBox::toggled, this, &MuPDFNGSettingsWidget::updateSignaturePreview);
    updateSignaturePreview();

    connect(
        m_mupdfsw->customRadioButton, &QRadioButton::toggled, m_mupdfsw->kcfg_dBCertificatePath, &QWidget::setEnabled);
    if (MuPDFNGSettings::useDefaultCertDB()) {
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

void MuPDFNGSettingsWidget::setupToolTips()
{
    // Translate and wrap once so each control and its label show identical help.
    const auto setToolTip = [](std::initializer_list<QWidget*> widgets, const QString& text) {
        const QString wrapped = Config::wrapToolTip(text);
        for (QWidget* widget : widgets)
            widget->setToolTip(wrapped);
    };
    setToolTip(
        { m_mupdfsw->kcfg_GraphicsAntialiasingBits, m_mupdfsw->labelGraphicsAA },
        i18n("Smooth the edges of vector graphics. Higher levels improve smoothness but require more processing."));
    setToolTip({ m_mupdfsw->kcfg_TextAntialiasingBits, m_mupdfsw->labelTextAA },
               i18n("Smooth the edges of text. Higher levels improve smoothness but require more processing."));
    setToolTip({ m_mupdfsw->kcfg_ImageRenderingQuality, m_mupdfsw->labelImageQuality },
               i18n("Choose the balance between image rendering speed and quality."));
    setToolTip({ m_mupdfsw->kcfg_ImageInterpolation }, i18n("Smooth images when scaling them to reduce jagged edges."));
    setToolTip({ m_mupdfsw->kcfg_OverprintSimulation },
               i18n("Preview how overlapping inks appear in a PDF. This affects viewing only; printing is unchanged."));
    setToolTip({ m_mupdfsw->kcfg_MemoryLimit, m_mupdfsw->labelMemoryUsage },
               i18n("Limit the worker’s rendering cache memory. This is not a limit on total process memory usage."));
    setToolTip({ m_mupdfsw->kcfg_IdleTrimLevel, m_mupdfsw->labelIdleTrim },
               i18n("Choose how eagerly the worker releases cached memory while idle."));
    setToolTip({ m_mupdfsw->kcfg_CancelObsoleteRenders },
               i18n("Stop rendering pages that are no longer needed. When disabled, rendering finishes before "
                    "obsolete results are discarded."));
    setToolTip({ m_mupdfsw->kcfg_SandboxEnforcement, m_mupdfsw->labelSandboxEnforcement },
               i18n("Strict refuses to process documents unless the worker sandbox is fully hardened on this host."));
    setToolTip({ m_mupdfsw->kcfg_NotifyDegradedSandbox },
               i18n("Show a notification when the worker sandbox cannot provide its full protection."));
    setToolTip({ m_mupdfsw->kcfg_HeuristicSynopsisEnabled },
               i18n("Infer a table of contents from document headings when an embedded table of contents is absent. "
                    "Generation may delay "
                    "opening. Changes apply when the document is next opened."));
    setToolTip({ m_mupdfsw->kcfg_PdfFormJavaScriptEnabled },
               i18n("Run JavaScript embedded in PDF forms. Disabled by default; enable only for documents you trust."));
    setToolTip({ m_mupdfsw->kcfg_OcrLanguage, m_mupdfsw->labelOcrLang },
               i18n("Select an installed Tesseract model for recognizing text in scanned pages. OCR is disabled when "
                    "no usable model is available."));
    setToolTip({ m_mupdfsw->kcfg_OcrQuality, m_mupdfsw->labelOcrQuality },
               i18n("Choose OCR resolution. Higher resolution requires more processing."));
    setToolTip(
        { m_mupdfsw->kcfg_OcrTriggerMode, m_mupdfsw->labelOcrTriggerMode },
        i18n("Automatically run OCR when the page contains fewer extracted characters than the selected threshold."));
    setToolTip({ m_mupdfsw->kcfg_OcrNotify }, i18n("Show notifications about OCR progress and results."));
    setToolTip({ m_mupdfsw->kcfg_UseDefaultCertDB, m_mupdfsw->defaultLabel },
               i18n("Use the default NSS certificate database for signing and validating signatures."));
    setToolTip({ m_mupdfsw->customRadioButton, m_mupdfsw->kcfg_dBCertificatePath },
               i18n("Use an existing local NSS certificate database directory for signing and validating signatures."));
    setToolTip(
        { m_mupdfsw->manageCertificatesButton },
        i18n("Open the certificate manager for the active NSS database to add, inspect, or delete certificates."));
    setToolTip({ m_mupdfsw->kcfg_UseOcsp },
               i18n("Check certificate revocation while validating signatures. An unreachable OCSP responder does not "
                    "invalidate signatures."));
    setToolTip({ m_mupdfsw->kcfg_SignatureProfile, m_mupdfsw->labelSignatureProfile },
               i18n("Choose the appearance of signatures added to the document."));
    setToolTip({ m_mupdfsw->kcfg_SignatureUseUtc },
               i18n("Render the signature appearance timestamp in UTC instead of local time."));
    setToolTip({ m_mupdfsw->kcfg_SignatureEmblem, m_mupdfsw->labelSignatureEmblem },
               i18n("Choose an emblem to include in the signature appearance."));
    setToolTip({ m_mupdfsw->kcfg_SignatureDrawBorder }, i18n("Draw a border around the signed signature appearance."));
    setToolTip({ m_mupdfsw->signaturePreviewLabel },
               i18n("Preview the selected signature profile, emblem, and time format using sample identity data."));
    setToolTip({ m_mupdfsw->kcfg_EpubFontSize, m_mupdfsw->labelEpubFontSize },
               i18n("Set the base font size for EPUB text."));
    setToolTip({ m_mupdfsw->kcfg_EpubFontFamily, m_mupdfsw->labelEpubFontFamily },
               i18n("Choose the base font family for EPUB text. Default uses the document’s font selection."));
    setToolTip({ m_mupdfsw->kcfg_EpubPageSize, m_mupdfsw->labelEpubPageSize },
               i18n("Choose the page dimensions used to lay out EPUB content."));
    setToolTip({ m_mupdfsw->customCssButton, m_mupdfsw->kcfg_EpubCustomCss },
               i18n("Customize EPUB appearance with CSS rules."));
    const QString databaseToolTip = m_mupdfsw->kcfg_dBCertificatePath->toolTip();
    m_mupdfsw->kcfg_dBCertificatePath->lineEdit()->setToolTip(databaseToolTip);
    m_mupdfsw->kcfg_dBCertificatePath->button()->setToolTip(databaseToolTip);
}

void MuPDFNGSettingsWidget::updateCustomCssButtonText()
{
    const bool hasCustomCss = !m_mupdfsw->kcfg_EpubCustomCss->encodedText().isEmpty();
    // encodedTextChanged fires per keystroke; only resize the button when the
    // configured/empty state actually flips.
    const QString text = hasCustomCss ? i18n("Custom CSS (configured)") : i18n("Custom CSS");
    if (m_mupdfsw->customCssButton->text() != text)
        m_mupdfsw->customCssButton->setText(text);
}

void MuPDFNGSettingsWidget::updateManageCertificatesButton()
{
    const QString databasePath = Plugin::Crypto::activeNssDatabasePath();
    const QString databaseLabel = databasePath.isEmpty() ? i18n("NSS database unavailable")
                                                         : CertificateManager::displayDatabasePath(databasePath);
    m_mupdfsw->manageCertificatesButton->setText(i18n("Manage Certificates - %1", databaseLabel));
}

void MuPDFNGSettingsWidget::updateSignaturePreview()
{
    const bool simple =
        m_mupdfsw->kcfg_SignatureProfile->currentData().toInt() == MuPDFNGSettings::EnumSignatureProfile::Simple;
    const bool useUtc = m_mupdfsw->kcfg_SignatureUseUtc->isChecked();
    const auto emblem = static_cast<Config::SignatureEmblem>(m_mupdfsw->kcfg_SignatureEmblem->currentData().toInt());
    QLabel* preview = m_mupdfsw->signaturePreviewLabel;
    // Point-size based application font: the preview text scales with the UI
    // font size and stays crisp on high-DPI screens via the ratio below.
    const QFont font = QApplication::font();
    const QImage image = Config::renderSignaturePreview(
        Config::buildSignaturePreview(simple, useUtc, QDateTime::currentDateTime(), emblem),
        font,
        preview->devicePixelRatioF());
    preview->setPixmap(QPixmap::fromImage(image));
}

void MuPDFNGSettingsWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // The constructor-time render may have sampled a 1.0 ratio before any
    // screen existed; re-render now that the ratio is real, and follow later
    // moves across screens with different scale factors.
    updateSignaturePreview();
    if (QWindow* window = windowHandle())
        connect(window,
                &QWindow::screenChanged,
                this,
                &MuPDFNGSettingsWidget::updateSignaturePreview,
                Qt::UniqueConnection);
}

MuPDFNGSettingsWidget::~MuPDFNGSettingsWidget()
{
    delete m_mupdfsw;
}

} // namespace Mu::Generator
