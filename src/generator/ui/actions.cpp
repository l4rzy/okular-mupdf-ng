// SPDX-FileCopyrightText: 2008 Pino Toscano <pino@kde.org>
// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/certmanager/manager_dialog.hpp"
#include "generator/mupdfng.hpp"
#include "generator/ui/window_menus.hpp"
#include "mupdfngsettings.h"
#include "plugin/crypto/nss.hpp"

#include <KActionCollection>
#include <KActionMenu>
#include <KCoreConfigSkeleton>
#include <KLocalizedString>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QIcon>
#include <QMenu>

namespace Mu::Generator {

namespace {

constexpr int ShortNoticeMs = 1500;

} // namespace

void Main::setupActions()
{
    setComponentName(QStringLiteral("okular_mupdfng"), i18n("MuPDF"));
    setupOcrActions();
    auto* previewToggle =
        new QAction(QIcon::fromTheme(QStringLiteral("view-preview")), i18n("Toggle Page Link Preview"), this);
    actionCollection()->addAction(QStringLiteral("mupdf_link_preview_toggle"), previewToggle);
    connect(previewToggle, &QAction::triggered, this, [this] {
        using PreviewMode = MuPDFNGSettings::EnumLinkPreviewMode;
        const bool enabled = MuPDFNGSettings::linkPreviewMode() == PreviewMode::Off;
        MuPDFNGSettings::setLinkPreviewMode(enabled ? PreviewMode::AutoSize : PreviewMode::Off);
        MuPDFNGSettings::self()->save();
        Q_EMIT notice(i18n("Page link preview: %1", enabled ? i18n("Adaptive Size") : i18n("Off")), ShortNoticeMs);
    });
    connect(MuPDFNGSettings::self(), &KCoreConfigSkeleton::configChanged, this, [this] {
        syncActions();
        observeOcrFocus();
    });
    auto* certificates =
        new QAction(QIcon::fromTheme(QStringLiteral("view-certificate")), i18n("Manage Certificates…"), this);
    actionCollection()->addAction(QStringLiteral("mupdf_manage_certificates"), certificates);
    KActionCollection::setShortcutsConfigurable(certificates, false);
    connect(certificates, &QAction::triggered, this, [] {
        CertificateManagerDialog dialog(Plugin::Crypto::activeNssDatabasePath(), QApplication::activeWindow());
        dialog.exec();
    });
    setupThemeActions();
    auto* documentSettings =
        new QAction(QIcon::fromTheme(QStringLiteral("configure")), i18n("Document Settings…"), this);
    actionCollection()->addAction(QStringLiteral("mupdf_document_settings"), documentSettings);
    KActionCollection::setShortcutsConfigurable(documentSettings, false);
    connect(documentSettings, &QAction::triggered, this, &Main::showDocumentSettings);
    auto* import =
        new QAction(QIcon::fromTheme(QStringLiteral("document-import")), i18n("Import Annotations from XFDF…"), this);
    actionCollection()->addAction(QStringLiteral("mupdf_import_xfdf"), import);
    KActionCollection::setShortcutsConfigurable(import, false);
    connect(import, &QAction::triggered, this, &Main::importXfdf);
    // All actions exist before their initial state and shortcut registration.
    syncActions();
    // A separate client presents one set of actions per window, even when
    // Okular leaves inactive tabs' generator clients in the GUI factory.
    setXML(QStringLiteral("<gui name=\"okular_mupdfng_document\"/>"));
    m_menuRegistration = std::make_unique<MenuRegistration>(guiClient());
}

void Main::setupOcrActions()
{
    auto* menu = new KActionMenu(QIcon::fromTheme(QStringLiteral("document-scan")), i18n("Tesseract OCR"), this);
    actionCollection()->addAction(QStringLiteral("mupdf_ocr"), menu);
    KActionCollection::setShortcutsConfigurable(menu, false);
    m_ocrActions = new QActionGroup(this);
    const auto addMode = [&](const QString& name, const QString& text, int mode) {
        auto* action = new QAction(text, m_ocrActions);
        action->setCheckable(true);
        action->setData(mode);
        KActionCollection::setShortcutsConfigurable(action, false);
        actionCollection()->addAction(name, action);
        menu->addAction(action);
    };
    using Mode = MuPDFNGSettings::EnumOcrTriggerMode;
    addMode(QStringLiteral("mupdf_ocr_inherit"), i18n("Use Global"), -1);
    addMode(QStringLiteral("mupdf_ocr_off"), i18n("Off"), Mode::Never);
    addMode(QStringLiteral("mupdf_ocr_auto"), i18n("Automatic"), Mode::Five);
    addMode(QStringLiteral("mupdf_ocr_on"), i18n("On"), Mode::Always);
    m_ocrUnavailableActions = new QActionGroup(this);
    for (const auto& text : { i18n("No OCR models available."), i18n("Please install a Tesseract language model.") })
        m_ocrUnavailableActions->addAction(menu->menu()->addAction(text));
    m_ocrUnavailableActions->setEnabled(false);
    const auto setMode = [this](int mode) {
        if (mode >= 0 && !m_document.settings.ocr) {
            MuPDFNGSettings::setOcrTriggerMode(mode);
            MuPDFNGSettings::self()->save();
        } else {
            auto settings = m_document.settings;
            if (mode < 0)
                settings.ocr.reset();
            else
                Config::setOcrTriggerMode(*settings.ocr, mode);
            if (!applyDocumentSettings(settings)) {
                syncOcrActions();
                return;
            }
        }
        const int menuMode = mode == Mode::Twenty ? Mode::Five : mode;
        for (QAction* action : m_ocrActions->actions()) {
            if (action->data().toInt() == menuMode) {
                Q_EMIT notice(i18n("Tesseract OCR: %1", KLocalizedString::removeAcceleratorMarker(action->text())),
                              ShortNoticeMs);
                break;
            }
        }
    };
    auto* toggle = new QAction(menu->icon(), i18n("Toggle Tesseract OCR"), this);
    actionCollection()->addAction(QStringLiteral("mupdf_ocr_toggle"), toggle);
    connect(toggle, &QAction::triggered, this, [this, setMode] {
        const int mode = m_document.settings.ocr ? Config::ocrTriggerModeFor(*m_document.settings.ocr)
                                                 : MuPDFNGSettings::ocrTriggerMode();
        setMode(m_menuRegistration->nextOcrMode(mode, m_document.settings.ocr.has_value()));
    });
    connect(menu->menu(), &QMenu::aboutToShow, this, &Main::syncOcrActions);
    connect(
        m_ocrActions, &QActionGroup::triggered, this, [setMode](QAction* action) { setMode(action->data().toInt()); });
}

KActionCollection* Main::actionCollection() const
{
    if (m_menuRegistration)
        m_menuRegistration->schedule();
    return KXMLGUIClient::actionCollection();
}

void Main::setupThemeActions()
{
    auto* menu = new KActionMenu(QIcon::fromTheme(QStringLiteral("color-management")), i18n("Document Theme"), this);
    actionCollection()->addAction(QStringLiteral("mupdf_theme"), menu);
    KActionCollection::setShortcutsConfigurable(menu, false);
    m_themeActions = new QActionGroup(this);
    const auto addTheme = [&](const QString& name, const QString& label, std::optional<Model::DocumentTheme> theme) {
        auto* action = new QAction(label, m_themeActions);
        action->setCheckable(true);
        action->setData(theme ? static_cast<int>(*theme) : -1);
        KActionCollection::setShortcutsConfigurable(action, false);
        actionCollection()->addAction(name, action);
        menu->addAction(action);
    };
    addTheme(QStringLiteral("mupdf_theme_inherit"), i18n("Use Global"), std::nullopt);
    addTheme(QStringLiteral("mupdf_theme_default"), i18n("Default"), Model::DocumentTheme::Default);
    addTheme(QStringLiteral("mupdf_theme_twilight"), i18n("Twilight"), Model::DocumentTheme::Twilight);
    addTheme(QStringLiteral("mupdf_theme_dusk"), i18n("Dusk"), Model::DocumentTheme::Dusk);
    addTheme(QStringLiteral("mupdf_theme_dark"), i18n("Dark"), Model::DocumentTheme::Dark);
    const auto setTheme = [this](std::optional<Model::DocumentTheme> theme) {
        if (theme && !m_document.settings.theme) {
            MuPDFNGSettings::setDocumentTheme(static_cast<int>(*theme));
            MuPDFNGSettings::self()->save();
        } else {
            auto settings = m_document.settings;
            settings.theme = theme;
            if (!applyDocumentSettings(settings)) {
                syncThemeActions();
                return;
            }
        }
        for (QAction* action : m_themeActions->actions()) {
            if (action->data().toInt() == (theme ? static_cast<int>(*theme) : -1)) {
                Q_EMIT notice(i18n("Document theme: %1", KLocalizedString::removeAcceleratorMarker(action->text())),
                              ShortNoticeMs);
                break;
            }
        }
    };
    connect(m_themeActions, &QActionGroup::triggered, this, [setTheme](QAction* action) {
        const int value = action->data().toInt();
        setTheme(value < 0 ? std::nullopt : std::optional(static_cast<Model::DocumentTheme>(value)));
    });
    const auto addToggle =
        [this, setTheme](const QString& name, const QString& label, const QString& icon, Model::DocumentTheme theme) {
            auto* action = new QAction(QIcon::fromTheme(icon), label, this);
            actionCollection()->addAction(name, action);
            connect(action, &QAction::triggered, this, [this, setTheme, theme] {
                const auto current = m_document.settings.theme.value_or(
                    static_cast<Model::DocumentTheme>(MuPDFNGSettings::documentTheme()));
                setTheme(current == theme ? Model::DocumentTheme::Default : theme);
            });
        };
    addToggle(QStringLiteral("mupdf_theme_toggle_dusk"),
              i18n("Toggle Dusk Mode"),
              QStringLiteral("draw-halfcircle1"),
              Model::DocumentTheme::Dusk);
    addToggle(QStringLiteral("mupdf_theme_toggle_dark"),
              i18n("Toggle Dark Mode"),
              QStringLiteral("lighttable"),
              Model::DocumentTheme::Dark);
    addToggle(QStringLiteral("mupdf_theme_toggle_twilight"),
              i18n("Toggle Twilight Mode"),
              QStringLiteral("draw-halfcircle2"),
              Model::DocumentTheme::Twilight);
    // Settings are shared by every open document. Refresh each worker after
    // the save completes, without re-entering the settings notification.
    connect(
        MuPDFNGSettings::self(),
        &KCoreConfigSkeleton::configChanged,
        this,
        [this] {
            if (const auto* currentDocument = document(); currentDocument && !m_document.settings.theme
                && m_settings.rendering.documentTheme
                    != static_cast<Model::DocumentTheme>(MuPDFNGSettings::documentTheme()))
                const_cast<Okular::Document*>(currentDocument)->reparseConfig();
        },
        Qt::QueuedConnection);
    connect(menu->menu(), &QMenu::aboutToShow, this, &Main::syncThemeActions);
}

void Main::syncThemeActions()
{
    const bool enabled = !m_okularPages.isEmpty() && !m_pageError.isActive();
    auto* menu = actionCollection()->action(QStringLiteral("mupdf_theme"));
    menu->setEnabled(enabled);
    menu->setText(m_document.settings.theme ? i18n("%1 (Local)", i18n("Document Theme")) : i18n("Document Theme"));
    actionCollection()->action(QStringLiteral("mupdf_theme_toggle_dusk"))->setEnabled(enabled);
    actionCollection()->action(QStringLiteral("mupdf_theme_toggle_dark"))->setEnabled(enabled);
    actionCollection()->action(QStringLiteral("mupdf_theme_toggle_twilight"))->setEnabled(enabled);
    m_themeActions->setEnabled(enabled);
    actionCollection()
        ->action(QStringLiteral("mupdf_theme_inherit"))
        ->setVisible(m_document.settings.theme.has_value());
    const int theme = static_cast<int>(
        m_document.settings.theme.value_or(static_cast<Model::DocumentTheme>(MuPDFNGSettings::documentTheme())));
    for (QAction* action : m_themeActions->actions())
        action->setChecked(action->data().toInt() == theme);
}

void Main::syncOcrActions()
{
    const bool enabled =
        m_document.type == Model::DocumentType::Pdf && !m_okularPages.isEmpty() && !m_pageError.isActive();
    const bool hasModels = !Config::installedOcrModels().isEmpty();
    auto* menu = actionCollection()->action(QStringLiteral("mupdf_ocr"));
    menu->setEnabled(enabled);
    menu->setText(m_document.settings.ocr ? i18n("%1 (Local)", i18n("Tesseract OCR")) : i18n("Tesseract OCR"));
    actionCollection()->action(QStringLiteral("mupdf_ocr_toggle"))->setEnabled(enabled && hasModels);
    m_ocrActions->setEnabled(enabled);
    for (QAction* action : m_ocrActions->actions()) {
        action->setEnabled(enabled && hasModels);
        action->setVisible(hasModels);
    }
    auto* inherit = actionCollection()->action(QStringLiteral("mupdf_ocr_inherit"));
    inherit->setVisible(m_document.settings.ocr.has_value());
    inherit->setEnabled(enabled);
    m_ocrUnavailableActions->setVisible(!hasModels);
    using Mode = MuPDFNGSettings::EnumOcrTriggerMode;
    const int configuredMode = m_document.settings.ocr ? Config::ocrTriggerModeFor(*m_document.settings.ocr)
                                                       : MuPDFNGSettings::ocrTriggerMode();
    const int menuMode = configuredMode == Mode::Twenty ? Mode::Five : configuredMode;
    for (QAction* action : m_ocrActions->actions())
        action->setChecked(action->data().toInt() == menuMode);
}

void Main::syncActions()
{
    syncThemeActions();
    syncOcrActions();
    auto* documentSettings = actionCollection()->action(QStringLiteral("mupdf_document_settings"));
    documentSettings->setEnabled(!m_okularPages.isEmpty() && workerReady());
    const auto& settings = m_document.settings;
    documentSettings->setText(settings.theme || settings.layout || settings.ocr ? i18n("Document Settings (Local)…")
                                                                                : i18n("Document Settings…"));
    actionCollection()->action(QStringLiteral("mupdf_import_xfdf"))->setEnabled(canImportXfdf());
    actionCollection()
        ->action(QStringLiteral("mupdf_link_preview_toggle"))
        ->setEnabled(!m_okularPages.isEmpty() && !m_pageError.isActive());
}

void Main::notifySetup(const QList<Okular::Page*>&, int)
{
    // Okular notifies observers when annotation editing permission changes.
    syncActions();
}

} // namespace Mu::Generator
