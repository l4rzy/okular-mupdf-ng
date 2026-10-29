// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/csseditor/editor.hpp"
#include "generator/mupdfng.hpp"

#include <KLocalizedString>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeDatabase>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace Mu::Generator {
namespace {

void addSettingRow(QVBoxLayout* layout, const QString& text, QWidget* control)
{
    auto* row = new QHBoxLayout;
    auto* label = new QLabel(text);
    label->setBuddy(control);
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet(QStringLiteral("border: none; border-bottom: 1px solid palette(midlight);"));
    row->addWidget(label);
    row->addWidget(line, 1);
    row->addWidget(control);
    layout->addLayout(row);
}

} // namespace

Config::OcrSettings Main::ocrSettings() const
{
    auto settings = Config::readOcrSettings();
    if (m_document.type == Model::DocumentType::Pdf && m_document.settings.ocr) {
        const int debounceMs = settings.debounceMs;
        settings = *m_document.settings.ocr;
        settings.debounceMs = debounceMs; // Hidden scheduler tuning stays global.
    }
    if (settings.language == QStringLiteral("-")) {
        settings.force = false;
        settings.autoTrigger = false;
    }
    return settings;
}

void Main::loadDocumentSettings(const QString& key)
{
    m_document.settingsKey = key;
    m_document.settings = Config::readDocumentSettings(key);
    const auto defaults = Config::readWorkerSettings();
    m_settings.rendering = defaults.rendering;
    m_settings.rendering.documentTheme = m_document.settings.theme.value_or(defaults.rendering.documentTheme);
    m_settings.startupEpub = m_document.settings.layout.value_or(defaults.startupEpub);
}

bool Main::applyDocumentSettings(const Config::DocumentSettings& settings)
{
    auto* doc = const_cast<Okular::Document*>(document());
    if (!doc || !workerReady() || m_okularPages.isEmpty() || m_document.settingsKey.isEmpty())
        return false;
    const auto previous = m_document.settings;
    if (settings == previous)
        return true;
    const auto layout = settings.layout.value_or(Config::readEpubSettings());
    const bool reload = Model::isReflowableDocument(m_document.type) && settings.layout != previous.layout
        && layout != m_settings.startupEpub;
    const QUrl url = doc->currentDocument();
    // Full pagination changes are supported for local books in this MVP.
    if (reload && (!url.isLocalFile() || m_formsDirty || m_annotationsDirty))
        return false;
    const QString key = m_document.settingsKey;
    if (!Config::writeDocumentSettings(key, settings)) {
        Q_EMIT error(i18n("Could not save document settings."), 5000);
        return false;
    }
    const bool ocrChanged = settings.ocr != previous.ocr;
    if (ocrChanged) {
        m_ocrController->reset();
        ++m_layerRevision; // Discard pending text completions from the previous OCR policy.
        refreshDocumentText(m_layerRevision.load());
    }
    m_document.settings = settings;
    if (!reload) {
        doc->reparseConfig();
        if (ocrChanged)
            doc->reloadDocument(); // Replace renders cancelled by the text revision change.
        syncActions();
        return true;
    }

    // Capture before close: it clears the retained password and source state.
    const QString path = url.toLocalFile();
    const QString password = m_document.password;
    const auto mime = QMimeDatabase().mimeTypeForFile(path);
    const int page = static_cast<int>(doc->currentPage());
    doc->closeDocument();
    if (doc->openDocument(path, url, mime, password) != Okular::Document::OpenSuccess) {
        Config::writeDocumentSettings(key, previous);
        doc->openDocument(path, url, mime, password);
        Q_EMIT error(i18n("Could not reload the book with the new layout."), 5000);
        return false;
    }
    doc->setViewportPage(std::clamp(page, 0, static_cast<int>(doc->pages()) - 1));
    return true;
}

void Main::showDocumentSettings()
{
    if (!document() || !workerReady() || m_okularPages.isEmpty())
        return;
    const QPointer<Main> guard(this);
    const QPointer<Okular::Document> target(const_cast<Okular::Document*>(document()));
    const QString key = m_document.settingsKey;
    const auto defaults = Config::readWorkerSettings();
    QDialog dialog(QApplication::activeWindow());
    dialog.setWindowTitle(i18n("MuPDF-NG Document Settings"));
    auto* main = new QVBoxLayout(&dialog);
    auto* name = new QLabel(m_document.name.left(512), &dialog);
    name->setObjectName(QStringLiteral("documentName"));
    auto nameFont = name->font();
    nameFont.setBold(true);
    name->setFont(nameFont);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    main->addWidget(name);
    main->addWidget(new QLabel(i18n("Settings saved for this document only."), &dialog));

    auto* themeGroup = new QGroupBox(i18n("Document Theme"), &dialog);
    auto* themeForm = new QVBoxLayout(themeGroup);
    auto* theme = new QComboBox(themeGroup);
    theme->setObjectName(QStringLiteral("documentTheme"));
    const QStringList themes { i18n("Default"), i18n("Twilight"), i18n("Dusk"), i18n("Dark") };
    theme->addItem(i18n("Use Global"), -1);
    for (int i = 0; i < themes.size(); ++i)
        theme->addItem(themes.at(i), i);
    theme->setCurrentIndex(m_document.settings.theme ? static_cast<int>(*m_document.settings.theme) + 1 : 0);
    addSettingRow(themeForm, i18n("Theme"), theme);
    main->addWidget(themeGroup);

    const bool ebook = Model::isReflowableDocument(m_document.type);
    auto* layoutGroup = new QGroupBox(i18n("Ebook layout"), &dialog);
    layoutGroup->setVisible(ebook);
    auto* layoutBox = new QVBoxLayout(layoutGroup);
    auto* inherit = new QCheckBox(i18n("Use Global"), layoutGroup);
    inherit->setObjectName(QStringLiteral("inheritLayout"));
    inherit->setChecked(!m_document.settings.layout);
    layoutBox->addWidget(inherit);
    auto* fields = new QWidget(layoutGroup);
    auto* form = new QVBoxLayout(fields);
    auto* size = new QSpinBox(fields);
    size->setObjectName(QStringLiteral("fontSize"));
    size->setRange(Limit::MinEpubFontSize, Limit::MaxEpubFontSize);
    size->setSuffix(i18n(" pt"));
    addSettingRow(form, i18n("Font Size"), size);
    auto* family = new QComboBox(fields);
    family->addItems({ i18n("Default"), i18n("Serif"), i18n("Sans-serif"), i18n("Monospace") });
    addSettingRow(form, i18n("Font Family"), family);
    auto* page = new QComboBox(fields);
    page->setObjectName(QStringLiteral("pageSize"));
    page->addItem(i18n("A5 (148 × 210 mm)"), static_cast<int>(Model::EpubPageSize::A5));
    page->addItem(i18n("6×9 (152 × 229 mm)"), static_cast<int>(Model::EpubPageSize::SixByNine));
    page->addItem(i18n("B5 (176 × 250 mm)"), static_cast<int>(Model::EpubPageSize::B5));
    page->addItem(i18n("Letter (216 × 279 mm)"), static_cast<int>(Model::EpubPageSize::Letter));
    addSettingRow(form, i18n("Page Size"), page);
    auto* cssToggle = new QPushButton(i18n("Custom CSS"), fields);
    cssToggle->setCheckable(true);
    form->addWidget(cssToggle);
    auto* css = new CssEditor(fields);
    css->setObjectName(QStringLiteral("customCss"));
    css->setVisible(false);
    form->addWidget(css);
    connect(cssToggle, &QPushButton::toggled, css, &QWidget::setVisible);
    const auto showLayout = [=](const Config::EpubSettings& layout) {
        size->setValue(layout.fontSize);
        family->setCurrentIndex(layout.fontFamily);
        page->setCurrentIndex(page->findData(layout.pageSize));
        css->setEncodedText(layout.customCssBase64);
    };
    showLayout(m_document.settings.layout.value_or(defaults.startupEpub));
    fields->setEnabled(!inherit->isChecked());
    connect(inherit, &QCheckBox::toggled, &dialog, [=](bool checked) {
        if (checked)
            showLayout(defaults.startupEpub);
        fields->setEnabled(!checked);
    });
    layoutBox->addWidget(fields);
    if (!target->currentDocument().isLocalFile()) {
        layoutGroup->setEnabled(false);
        layoutGroup->setToolTip(i18n("Reopen this book from a local file to change its layout."));
    }
    main->addWidget(layoutGroup);
    const bool pdf = m_document.type == Model::DocumentType::Pdf;
    const auto ocrDefaults = Config::readOcrSettings();
    auto* ocrGroup = new QGroupBox(i18n("Tesseract OCR"), &dialog);
    ocrGroup->setVisible(pdf);
    auto* ocrBox = new QVBoxLayout(ocrGroup);
    auto* inheritOcr = new QCheckBox(i18n("Use Global"), ocrGroup);
    inheritOcr->setObjectName(QStringLiteral("inheritOcr"));
    inheritOcr->setChecked(!m_document.settings.ocr);
    ocrBox->addWidget(inheritOcr);
    auto* ocrFields = new QWidget(ocrGroup);
    auto* ocrForm = new QVBoxLayout(ocrFields);
    auto* language = new QComboBox(ocrFields);
    language->setObjectName(QStringLiteral("ocrLanguage"));
    language->addItem(i18n("Off"), QStringLiteral("-"));
    for (const auto& model : Config::installedOcrModels()) {
        const QString code = model.chopped(QStringLiteral(".traineddata").size());
        language->addItem(model, code);
    }
    addSettingRow(ocrForm, i18n("Language"), language);
    auto* quality = new QComboBox(ocrFields);
    quality->setObjectName(QStringLiteral("ocrQuality"));
    quality->addItem(i18n("Speed (150dpi)"), 150);
    quality->addItem(i18n("Balanced (225dpi)"), 225);
    quality->addItem(i18n("Accuracy (300dpi)"), 300);
    addSettingRow(ocrForm, i18n("Quality"), quality);
    auto* mode = new QComboBox(ocrFields);
    mode->setObjectName(QStringLiteral("ocrMode"));
    mode->addItems({ i18n("0 (Never)"), i18n("5 characters"), i18n("20 characters"), i18n("∞ (Always)") });
    addSettingRow(ocrForm, i18n("Trigger Mode"), mode);
    auto* notify = new QCheckBox(i18n("Show OCR notifications"), ocrFields);
    notify->setObjectName(QStringLiteral("ocrNotify"));
    ocrForm->addWidget(notify);
    const auto showOcr = [=](const Config::OcrSettings& settings) {
        int index = language->findData(settings.language);
        if (index < 0) {
            language->addItem(i18n("%1 (unavailable)", settings.language + QStringLiteral(".traineddata")),
                              settings.language);
            index = language->count() - 1;
        }
        language->setCurrentIndex(index);
        quality->setCurrentIndex(quality->findData(settings.dpi));
        mode->setCurrentIndex(Config::ocrTriggerModeFor(settings));
        notify->setChecked(settings.notify);
    };
    showOcr(m_document.settings.ocr.value_or(ocrDefaults));
    ocrFields->setEnabled(!inheritOcr->isChecked());
    connect(inheritOcr, &QCheckBox::toggled, &dialog, [=](bool checked) {
        if (checked)
            showOcr(ocrDefaults);
        ocrFields->setEnabled(!checked);
    });
    ocrBox->addWidget(ocrFields);
    main->addWidget(ocrGroup);
    auto* note = new QLabel(
        ebook ? i18n("Layout changes reload the book and may change page numbers. Theme changes apply immediately.")
              : i18n("OCR and theme changes apply immediately."),
        &dialog);
    note->setWordWrap(true);
    main->addWidget(note);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    main->addWidget(buttons);
    auto applied = m_document.settings;
    const auto readSettings = [&] {
        auto settings = applied;
        settings.theme = theme->currentIndex() == 0
            ? std::nullopt
            : std::optional(static_cast<Model::DocumentTheme>(theme->currentData().toInt()));
        if (ebook && layoutGroup->isEnabled()) {
            settings.layout = inherit->isChecked()
                ? std::nullopt
                : std::optional(Config::EpubSettings {
                      size->value(), family->currentIndex(), page->currentData().toInt(), css->encodedText() });
        }
        if (pdf) {
            settings.ocr.reset();
            if (!inheritOcr->isChecked()) {
                Config::OcrSettings ocr;
                ocr.language = language->currentData().toString();
                ocr.dpi = quality->currentData().toInt();
                ocr.notify = notify->isChecked();
                Config::setOcrTriggerMode(ocr, mode->currentIndex());
                settings.ocr = ocr;
            }
        }
        return settings;
    };
    const auto updateButtons = [&] {
        themeGroup->setTitle(theme->currentIndex() == 0 ? i18n("Document Theme")
                                                        : i18n("%1 (Local)", i18n("Document Theme")));
        ocrGroup->setTitle(inheritOcr->isChecked() ? i18n("Tesseract OCR") : i18n("%1 (Local)", i18n("Tesseract OCR")));
        buttons->button(QDialogButtonBox::Apply)->setEnabled(readSettings() != applied);
    };
    connect(theme, &QComboBox::currentIndexChanged, &dialog, updateButtons);
    connect(inherit, &QCheckBox::toggled, &dialog, updateButtons);
    connect(size, &QSpinBox::valueChanged, &dialog, updateButtons);
    connect(family, &QComboBox::currentIndexChanged, &dialog, updateButtons);
    connect(page, &QComboBox::currentIndexChanged, &dialog, updateButtons);
    connect(css, &CssEditor::encodedTextChanged, &dialog, updateButtons);
    connect(inheritOcr, &QCheckBox::toggled, &dialog, updateButtons);
    connect(language, &QComboBox::currentIndexChanged, &dialog, updateButtons);
    connect(quality, &QComboBox::currentIndexChanged, &dialog, updateButtons);
    connect(mode, &QComboBox::currentIndexChanged, &dialog, updateButtons);
    connect(notify, &QCheckBox::toggled, &dialog, updateButtons);
    const auto apply = [&] {
        if (!guard || !target || guard->document() != target || guard->m_document.settingsKey != key) {
            dialog.reject();
            return false;
        }
        const auto settings = readSettings();
        if (settings != applied && !guard->applyDocumentSettings(settings))
            return false;
        applied = settings;
        updateButtons();
        return true;
    };
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, apply);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (apply())
            dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateButtons();
    dialog.resize(500, dialog.sizeHint().height());
    dialog.exec();
}

} // namespace Mu::Generator
