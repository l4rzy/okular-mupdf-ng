// SPDX-FileCopyrightText: 2008 Pino Toscano <pino@kde.org>
// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/mupdfng.hpp"

#include <okular/core/fileprinter.h>
#include <okular/core/page.h>
#include <okular/core/textpage.h>

#include <KConfigDialog>
#include <KLocalizedString>
#include <KMessageBox>
#include <KPluginFactory>
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QLocale>
#include <QMessageBox>
#include <QMutexLocker>
#include <QPageLayout>
#include <QPainter>
#include <QPrinter>
#include <QTemporaryFile>
#include <QTextStream>
#include <unordered_map>

#include "generator/config/settingswidget.hpp"
#include "generator/config/settings.hpp"
#include "generator/conversion/annotation.hpp"
#include "generator/conversion/document.hpp"
#include "generator/conversion/text.hpp"
#include "generator/proxy/annotation.hpp"
#include "generator/proxy/form/checkbox.hpp"
#include "generator/proxy/form/choice.hpp"
#include "generator/proxy/form/coordinator.hpp"
#include "generator/proxy/form/push_button.hpp"
#include "generator/proxy/form/radio_button.hpp"
#include "generator/proxy/form/radio_grouping.hpp"
#include "generator/proxy/form/text.hpp"
#include "mupdfngsettings.h"
#include "plugin/crypto/nss.hpp"
#include "plugin/ocr/ocr.hpp"
#include "plugin/util/signature_image.hpp"
#include "plugin/util/temp_dir.hpp"
#include "shared/compat.hpp"
#include "shared/logging.hpp"
#include "shared/transport/common.hpp"

K_PLUGIN_CLASS_WITH_JSON(::Mu::Generator::Main, "manifest.json")

namespace Mu::Generator {

// Okular Generator Func: creates the generator and initializes worker services.
Main::Main(QObject* parent, const QVariantList& args)
    : Generator(parent, args)
    , m_annotationProxy(&m_worker)
    , m_certStore(std::make_unique<Proxy::CertificateStore>())
    , m_settings(Config::readWorkerSettings())
{
    // Step 1: Advertise only the Okular services implemented by this adapter.
    setFeature(Threaded);
    // Printing spools PDF through FilePrinter (lpr/CUPS), not QPainter.
    setFeature(PrintPostscript);
    setFeature(TextExtraction);
    setFeature(ReadRawData);
    setFeature(FontInfo);
    setFeature(PrintPostscript);
    setFeature(PrintToFile);
    setFeature(TiledRendering);
    setFeature(SwapBackingFile);

    // Step 2: Build the UI-side adapters before the worker can emit events.
    const QString certDbPath = Config::readCertificateDatabasePath(Plugin::Crypto::defaultSystemNssDbPath());
    (void)Plugin::Crypto::initializeNss(certDbPath);
    m_ocrController = std::make_unique<Plugin::OCR::Controller>(&m_worker, this);
    m_formCoordinator = std::make_unique<Proxy::Form::Coordinator>(
        &m_worker, [this](const std::vector<Okular::FormField*>& fields, const std::vector<int>& affectedPages) {
            m_formsDirty = true;
            // Form mutations happen in the worker. Ask Okular to refresh the
            // affected widgets and page pixmaps after the proxy state changes.
            const Okular::Document* currentDocument = document();
            if (!currentDocument)
                return;
            auto* mutableDocument = const_cast<Okular::Document*>(currentDocument);
            for (Okular::FormField* field : fields) {
                if (field)
                    Q_EMIT mutableDocument->refreshFormWidget(field);
            }
            for (int pageIndex : affectedPages) {
                if (pageIndex >= 0 && pageIndex < m_okularPages.size())
                    mutableDocument->refreshPixmaps(pageIndex);
            }
        });

    // Step 3: Start the isolated worker. The worker binary path is embedded at compile time by CMake.
    // WorkerClient::start() will also try PATH and install-dir fallbacks.
    if (!m_worker.start(QString(), Config::readTessDataDirectories())) {
        MU_LOG(warning, "Mu::Generator::Main", "failed to start okular-mupdf-worker");
    }
    // Step 4: Invalidate asynchronous UI state before handling worker failure.
    connect(&m_worker, &Plugin::WorkerClient::workerDied, this, [this](int code) {
        MU_LOG(
            warning, "Mu::Generator::Main", std::string("okular-mupdf-worker died with code ") + std::to_string(code));
        m_ocrController->reset();
        if (m_formsDirty)
            failClosed(i18n(
                "The document renderer stopped while unsaved form changes were present. Those changes were lost."));
    });
    connect(
        &m_worker,
        &Plugin::WorkerClient::pageLinksReady,
        this,
        [this](quint64, std::vector<Model::PageLinks> pages, bool resourceLimited, QString error) {
            for (const auto& pageLinks : pages) {
                if (pageLinks.page >= 0 && pageLinks.page < m_okularPages.size())
                    m_okularPages.at(pageLinks.page)->setObjectRects(Conversion::objectRects(pageLinks.links));
            }
            if (!error.isEmpty()) {
                MU_LOG(warning,
                       "Mu::Generator::Main",
                       std::string("asynchronous page-link loading stopped: ") + error.toStdString());
            } else if (resourceLimited) {
                MU_LOG(warning, "Mu::Generator::Main", "asynchronous page-link loading hit its resource limit");
            } else {
                MU_LOG(debug, "Mu::Generator::Main", "asynchronous page-link loading completed");
            }
        },
        Qt::QueuedConnection);
    connect(&m_worker, &Plugin::WorkerClient::workerRestarted, this, [this] {
        if (m_placeholder.isActive())
            return;
        // Defer recovery to avoid reopening the document from inside a worker
        // lifecycle signal; the queued call runs on the generator's Qt thread.
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (!reopenWorkerDocument())
                    return;

                const Okular::Document* currentDocument = document();
                if (!currentDocument)
                    return;
                const int page = static_cast<int>(currentDocument->currentPage());
                if (page >= 0 && page < m_okularPages.size())
                    const_cast<Okular::Document*>(currentDocument)->refreshPixmaps(page);
            },
            Qt::QueuedConnection);
    });
    connect(
        &m_worker,
        &Plugin::WorkerClient::workerUnavailable,
        this,
        [this] {
            MU_LOG(critical, "Mu::Generator::Main", "worker restart limit reached; displaying error image");
            failClosed(i18n("The document renderer stopped responding. Restart Okular to try again."));
        },
        Qt::QueuedConnection);
    connect(m_ocrController.get(),
            &Plugin::OCR::Controller::completed,
            this,
            [this](int page,
                   QVector<Plugin::Caching::OCR::CacheItem> boxes,
                   Plugin::OCR::Controller::CompletionSource source) {
                QMutexLocker locker(userMutex());
                if (page < 0 || page >= m_okularPages.size())
                    return;
                auto* textPage = new Okular::TextPage();
                for (const auto& box : boxes)
                    textPage->append(box.ch, Okular::NormalizedRect(box.l, box.t, box.r, box.b));
                Q_EMIT signalTextGenerationDone(m_okularPages.at(page), textPage);
                if (Config::readOcrSettings().notify) {
                    const QString message = source == Plugin::OCR::Controller::CompletionSource::CacheLoaded
                        ? i18n("OCR cache loaded for page %1", page + 1)
                        : i18n("OCR complete for page %1", page + 1);
                    Q_EMIT notice(message, 1500);
                }
            });
    connect(m_ocrController.get(), &Plugin::OCR::Controller::started, this, [this](int page) {
        if (Config::readOcrSettings().notify)
            Q_EMIT notice(i18n("Running OCR on page %1...", page + 1), 2000);
    });
}

// Okular Generator Func: stops worker activity and releases generator resources.
Main::~Main()
{
    // Cancel callbacks before removing queued events or releasing UI-owned data.
    m_ocrController->reset();
    QCoreApplication::removePostedEvents(this);
    {
        QMutexLocker locker(userMutex());
        clearEmbeddedFilesCache();
    }
    m_worker.stop();
}

// Okular Generator Func: reads the paper color Okular's accessibility settings
// request through documentMetaData (white when the Paper render mode is off)
// and records it for the next settings push.
void Main::refreshPaperColor()
{
    const QColor color = documentMetaData(Okular::Generator::PaperColorMetaData, true).value<QColor>();
    m_paperColorRgb =
        color.isValid() ? static_cast<std::uint32_t>(qRgb(color.red(), color.green(), color.blue())) : 0xFFFFFFu;
}

// Okular Generator Func: reloads settings and sends rendering changes to the worker.
bool Main::reparseConfig()
{
    Config::reloadSettings();
    const Config::WorkerSettings settings = Config::readWorkerSettings();
    updateSettingRestartState();
    const std::uint32_t previousPaperColorRgb = m_paperColorRgb;
    refreshPaperColor();
    const bool paperColorChanged = previousPaperColorRgb != m_paperColorRgb;
    const bool changed = Config::renderingOutputChanged(m_settings.rendering, settings.rendering) || paperColorChanged;
    const bool settingsChanged = settings.rendering != m_settings.rendering || paperColorChanged;
    m_settings.rendering = settings.rendering;

    // Propagate changed settings to the worker process.
    if (settingsChanged && m_worker.isConnected()
        && !m_worker.setSettings(m_settings.documentSettings(m_paperColorRgb)))
        MU_LOG(warning, "Mu::Generator::Main", "failed to apply settings after configuration reload");

    // Re-evaluate sandbox enforcement for the active document. Blocking
    // immediately withholds the document (renders become placeholders and the
    // worker copy is dropped); unblocking is deferred: deactivate() queues a
    // full close/open cycle, and the placeholder stays active until the
    // successful reopen clears it in initPages(), so no render can reach the
    // worker while the document is still closed there.
    const bool newBlocked = sandboxGated();
    if (newBlocked != m_placeholder.isActive() && document()) {
        MU_LOG(debug,
               "Mu::Generator::Main",
               std::string("sandbox enforcement changed: ") + (newBlocked ? "relaxed -> strict" : "strict -> relaxed"));
        if (newBlocked) {
            // Side effects run after activate() publishes the flag, so any
            // render triggered by them observes the active state.
            m_placeholder.activate(SandboxGate::guidanceMessage(m_worker.sandboxStatus()),
                                   Placeholder::Reason::SandboxGate);
            m_worker.close();
            clearPlaceholderDerivedState();
        } else if (m_placeholder.deactivate()) {
            reopenWithheldDocument();
        }
    }

    return changed;
}

// Okular Generator Func: adds the generator settings page to Okular.
void Main::addPages(KConfigDialog* dialog)
{
    MuPDFSettingsWidget* w = new MuPDFSettingsWidget(dialog);
    dialog->addPage(
        w, MuPDFSettings::self(), i18n("MuPDF-NG"), QStringLiteral("okular-mupdf-ng"), i18n("MuPDF-NG Configuration"));
    w->updateCustomCssButtonText();
    connect(dialog, &KConfigDialog::settingsChanged, this, [this, w] {
        updateSettingRestartState();
        if (!m_restartState.required)
            return;
        QMessageBox::information(w, i18n("Restart needed"), i18n("You need to restart Okular after this change."));
    });
}

void Main::updateSettingRestartState()
{
    // Worker process lifetime settings must not be applied to a running worker.
    const QString certDbPath = Config::readCertificateDatabasePath(Plugin::Crypto::defaultSystemNssDbPath());
    m_restartState.required =
        Config::readEpubSettings() != m_settings.startupEpub || !Plugin::Crypto::isNssDatabaseActive(certDbPath);
}

// Converts worker page information into Okular pages and page metadata.
Okular::Document::OpenResult Main::initPages(QVector<Okular::Page*>& pages,
                                             QList<Plugin::WorkerClient::PageInfo>& pageInfos,
                                             const QString& password)
{
    // Step 1: Reset state that is scoped to the previous document generation.
    m_document.password = password;
    m_placeholder.reset();
    m_ocrController->reset();

    // Step 2: Obtain document identity before constructing Okular-owned pages.
    const auto info =
        m_worker.getDocumentInfo({ QStringLiteral("title"), QStringLiteral("hash"), QStringLiteral("repaired") });
    m_document.type = Model::documentTypeFromMime(info.mimeType);
    warnIfRepairedDocument(info);

    // Signature validation reads the original PDF bytes, which may come from a
    // file or from the in-memory source retained for worker recovery.
    QFile signatureFile(m_document.sourcePath);
    QBuffer signatureBuffer(&m_document.sourceData);
    QIODevice* signatureSource = nullptr;
    if (m_document.type == Model::DocumentType::Pdf) {
        if (!m_document.sourcePath.isEmpty() && signatureFile.open(QIODevice::ReadOnly))
            signatureSource = &signatureFile;
        else if (!m_document.sourceData.isEmpty() && signatureBuffer.open(QIODevice::ReadOnly))
            signatureSource = &signatureBuffer;
    }
    std::vector<Proxy::Form::RadioGroupMember> radioMembers;
    std::unordered_map<int, Proxy::Form::RadioButton*> radioProxies;

    const bool canValidateSignatures = signatureSource != nullptr;
    for (Plugin::WorkerClient::PageInfo& pageInfo : pageInfos) {
        const QSizeF s(pageInfo.geometry.widthPoints * dpi().width() / 72.0,
                       pageInfo.geometry.heightPoints * dpi().height() / 72.0);
        Okular::Page* okularPage =
            new Okular::Page(static_cast<uint>(pageInfo.number), s.width(), s.height(), Okular::Rotation0);
        okularPage->setDuration(pageInfo.geometry.duration);
        if (!pageInfo.geometry.label.empty())
            okularPage->setLabel(QString::fromStdString(pageInfo.geometry.label));

        for (const Model::Annotation& ad : pageInfo.annotations) {
            if (auto ann = Conversion::fromModel(ad)) {
                okularPage->addAnnotation(ann.release());
            }
        }

        QList<Okular::FormField*> formFields;

        for (Model::SignatureField& sfd : pageInfo.signatureFields) {
            if (canValidateSignatures)
                Plugin::Crypto::validateDetachedPdfSignature(sfd, *signatureSource, MuPDFSettings::self()->useOcsp());
            auto* sig = new Proxy::Form::Signature(sfd.objectNumber, std::move(sfd), &m_worker);
            formFields.append(sig);
        }
        for (Model::FormField& field : pageInfo.formFields) {
            switch (field.type) {
            case Model::FormFieldType::Text: {
                auto* proxy = new Proxy::Form::Text(field.pdfObjectNumber, std::move(field), m_formCoordinator.get());
                m_formCoordinator->registerField(proxy->model().handle, proxy);
                formFields.append(proxy);
                break;
            }
            case Model::FormFieldType::CheckBox: {
                auto* proxy =
                    new Proxy::Form::CheckBox(field.pdfObjectNumber, std::move(field), m_formCoordinator.get());
                m_formCoordinator->registerField(proxy->model().handle, proxy);
                formFields.append(proxy);
                break;
            }
            case Model::FormFieldType::RadioButton: {
                auto* radio =
                    new Proxy::Form::RadioButton(field.pdfObjectNumber, std::move(field), m_formCoordinator.get());
                m_formCoordinator->registerField(radio->model().handle, radio);
                formFields.append(radio);

                radioMembers.push_back({ radio->id(), radio->model().groupName, radio->model().fieldObjectNumber });
                radioProxies.emplace(radio->id(), radio);
                break;
            }
            case Model::FormFieldType::PushButton: {
                auto* button =
                    new Proxy::Form::PushButton(field.pdfObjectNumber, std::move(field), m_formCoordinator.get());
                m_formCoordinator->registerField(button->model().handle, button);
                formFields.append(button);
                break;
            }
            case Model::FormFieldType::ComboBox:
            case Model::FormFieldType::ListBox: {
                auto* proxy = new Proxy::Form::Choice(field.pdfObjectNumber, std::move(field), m_formCoordinator.get());
                m_formCoordinator->registerField(proxy->model().handle, proxy);
                formFields.append(proxy);
                break;
            }
            }
        }

        okularPage->setObjectRects(Conversion::objectRects(pageInfo.links));
        if (!formFields.isEmpty()) {
            okularPage->setFormFields(formFields);
        }

        pages.append(okularPage);
    }

    // Radio siblings can span pages, so resolve their groups after every proxy
    // has been created.
    for (const auto& [id, siblings] : Proxy::Form::radioSiblingIds(radioMembers)) {
        auto proxy = radioProxies.find(id);
        if (proxy != radioProxies.end()) {
            QList<int> siblingIds;
            for (int sibling : siblings)
                siblingIds.append(sibling);
            proxy->second->setSiblings(siblingIds);
        }
    }

    m_okularPages = pages;
    m_formsDirty = false;
    m_formCoordinator->setAvailable(true);
    m_document.hash = QString::fromStdString(info.values.contains("hash") ? info.values.at("hash") : std::string());
    const QString title =
        QString::fromStdString(info.values.contains("title") ? info.values.at("title") : std::string());
    if (!title.trimmed().isEmpty())
        m_document.name = title;
    return Okular::Document::OpenSuccess;
}

// Okular Generator Func: opens a file and creates its Okular pages.
Okular::Document::OpenResult
Main::loadDocumentWithPassword(const QString& fileName, QVector<Okular::Page*>& pages, const QString& password)
{
    if (!m_worker.isConnected())
        return Okular::Document::OpenError;
    if (sandboxGated()) {
        // Retained so a Strict-gated document can still be reopened with it
        // once enforcement relaxes.
        m_document.password = password;
        return loadBlockedPlaceholderDocument(pages);
    }
    const auto docType = Config::documentTypeForFile(fileName);
    if (docType == Model::DocumentType::Unknown)
        return Okular::Document::OpenError;
    refreshPaperColor();
    QList<Plugin::WorkerClient::PageInfo> workerPages;
    if (!m_worker.setSettings(m_settings.documentSettings(m_paperColorRgb)))
        return Okular::Document::OpenError;
    const auto openStatus = m_worker.open(fileName, password, workerPages, docType);
    if (openStatus == Model::OpenStatus::NeedsPassword)
        return Okular::Document::OpenNeedsPassword;
    if (openStatus != Model::OpenStatus::Success)
        return Okular::Document::OpenError;
    // Retain the source only after the worker accepted it; restart recovery
    // must never reopen a failed or partially initialized document.
    m_document.name = QFileInfo(fileName).fileName();
    m_document.sourcePath = fileName;
    m_document.sourceData.clear();
    return initPages(pages, workerPages, password);
}

// Okular Generator Func: opens memory data and creates its Okular pages.
Okular::Document::OpenResult Main::loadDocumentFromDataWithPassword(const QByteArray& fileData,
                                                                    QVector<Okular::Page*>& pages,
                                                                    const QString& password)
{
    if (!m_worker.isConnected())
        return Okular::Document::OpenError;
    if (sandboxGated()) {
        m_document.password = password;
        return loadBlockedPlaceholderDocument(pages);
    }
    QList<Plugin::WorkerClient::PageInfo> workerPages;
    const auto docType = Config::documentTypeForData(fileData);
    if (docType == Model::DocumentType::Unknown)
        return Okular::Document::OpenError;
    refreshPaperColor();
    if (!m_worker.setSettings(m_settings.documentSettings(m_paperColorRgb)))
        return Okular::Document::OpenError;
    const auto openStatus = m_worker.openData(fileData, password, workerPages, docType);
    if (openStatus == Model::OpenStatus::NeedsPassword)
        return Okular::Document::OpenNeedsPassword;
    if (openStatus != Model::OpenStatus::Success)
        return Okular::Document::OpenError;
    // Keep in-memory documents for the same restart path used by file sources.
    m_document.name =
        docType == Model::DocumentType::Epub ? QStringLiteral("document.epub") : QStringLiteral("document.pdf");
    m_document.sourcePath.clear();
    m_document.sourceData = fileData;
    return initPages(pages, workerPages, password);
}

// Reports xref-repair state to the user, mirroring the poppler generator's
// xrefReconstructionHandler. MuPDF repairs broken documents silently, so the
// worker exposes the state as metadata and the generator turns it into the
// same warning the poppler backend shows.
void Main::warnIfRepairedDocument(const Model::DocumentMetadata& info)
{
    const auto repaired = info.values.find("repaired");
    if (repaired == info.values.end() || repaired->second != "true")
        return;
    Q_EMIT warning(
        i18n("Some errors were found in the document, Okular might not be able to show the content correctly"), 5000);
}

// Builds the "Using MuPDF ..." description shown by Okular's About dialog,
// mirroring the poppler generator's GeneratorExtraDescription. The runtime
// version of the worker binary is authoritative; when it diverges from the
// MuPDF version pinned in cmake/mupdf.version, both are disclosed.
QString Main::generatorExtraDescription() const
{
    QString runtimeVersion;
    if (m_worker.isConnected()) {
        const auto info = m_worker.getDocumentInfo({ QStringLiteral("engineVersion") });
        const auto it = info.values.find("engineVersion");
        if (it != info.values.end())
            runtimeVersion = QString::fromStdString(it->second);
    }

    QString result;
    if (runtimeVersion.isEmpty()) {
        result = i18n("Using MuPDF %1", QString::fromStdString(std::string(Mu::MUPDF_VERSION)));
    } else if (runtimeVersion.toStdString() == Mu::MUPDF_VERSION) {
        result = i18n("Using MuPDF %1", runtimeVersion);
    } else {
        result = i18n("Using MuPDF %1\nBuilt against MuPDF %2",
                      runtimeVersion,
                      QString::fromStdString(std::string(Mu::MUPDF_VERSION)));
    }

    const Model::SandboxStatus status = m_worker.sandboxStatus();
    if (status.isFullyHardened()) {
        result += QStringLiteral("\n") + i18n("Worker is fully hardened 🟢");
    } else if (status.isPartiallyActive()) {
        result += QStringLiteral("\n") + i18n("Worker is partially hardened 🟡");
    } else {
        result += QStringLiteral("\n") + i18n("Worker is unconfined 🔴");
    }

    return result;
}

// Reports whether Strict enforcement currently blocks the not-fully-hardened worker.
bool Main::sandboxGated() const
{
    return Config::readSandboxEnforcement() == Config::SandboxEnforcement::Strict
        && !m_worker.sandboxStatus().isFullyHardened();
}

// Loads a single synthetic placeholder page while Strict enforcement withholds
// the real document from the worker. The page is only a display canvas for the
// guidance message; switching enforcement to Relaxed reloads the real document
// through Okular.
Okular::Document::OpenResult Main::loadBlockedPlaceholderDocument(QVector<Okular::Page*>& pages)
{
    // Side effects run after activate() publishes the flag; at load time the
    // worker has no document and no derived state, so both are best-effort.
    m_placeholder.activate(SandboxGate::guidanceMessage(m_worker.sandboxStatus()), Placeholder::Reason::SandboxGate);
    m_worker.close();
    clearPlaceholderDerivedState();
    pages.append(SandboxGate::withheldPage(dpi().width(), dpi().height()));
    m_okularPages = pages;
    return Okular::Document::OpenSuccess;
}

// Must be called while userMutex() is held; drops cached UI objects derived
// from the worker document.
void Main::clearWorkerDerivedState()
{
    m_synopsis.reset();
    clearEmbeddedFilesCache();
    if (m_formCoordinator) {
        m_formCoordinator->clear();
        m_formCoordinator->setAvailable(false);
    }
}

// Drops worker-derived UI state while the placeholder withholds the document,
// then refreshes pages so placeholder images replace stale real pixmaps.
void Main::clearPlaceholderDerivedState()
{
    m_ocrController->reset();
    m_formsDirty = false;
    {
        QMutexLocker locker(userMutex());
        clearWorkerDerivedState();
    }
    for (int page = 0; page < m_okularPages.size(); ++page) {
        clearPageDisplayState(page);
        const Okular::Document* currentDocument = document();
        if (currentDocument)
            const_cast<Okular::Document*>(currentDocument)->refreshPixmaps(page);
    }
}

// Queued because Okular invokes reparseConfig() synchronously; closing and
// reopening the document from inside that call would re-enter Okular::Document.
void Main::reopenWithheldDocument()
{
    QMetaObject::invokeMethod(this, [this] { reopenWithheldDocumentInternal(); }, Qt::QueuedConnection);
}

// Executes the queued close/open cycle on a clean stack.
void Main::reopenWithheldDocumentInternal()
{
    const auto result =
        SandboxGate::reopenLocalDocument(const_cast<Okular::Document*>(document()), m_document.password);
    if (result == SandboxGate::ReopenResult::NotLocal)
        Q_EMIT warning(i18n("Switched to Relaxed enforcement. Please reopen the document manually."), 0);
}

// Okular Generator Func: replaces the backing file while preserving page state.
Okular::Generator::SwapBackingFileResult Main::swapBackingFile(const QString& newFileName,
                                                               QList<Okular::Page*>& newPages)
{
    if (m_placeholder.isActive())
        return SwapBackingFileError;

    // Okular adopts temporary replacement-page contents into these existing
    // pages, then deletes the replacements after the swap.
    const QVector<Okular::Page*> currentPages = m_okularPages;
    const QString password = m_document.password;
    doCloseDocument();

    QVector<Okular::Page*> loadedPages;
    if (loadDocumentWithPassword(newFileName, loadedPages, password) != Okular::Document::OpenSuccess) {
        qDeleteAll(loadedPages);
        failClosed(i18n("The document could not be reloaded after saving. Restart Okular to continue."));
        return SwapBackingFileError;
    }
    for (Okular::Page* page : std::as_const(loadedPages))
        newPages.append(page);
    m_okularPages = currentPages;
    // Okular transfers the old pixmap cache to the replacement page internals.
    // Refresh it after that transfer so it reflects the saved worker document.
    QMetaObject::invokeMethod(
        this,
        [this] {
            const Okular::Document* currentDocument = document();
            if (!currentDocument)
                return;
            auto* mutableDocument = const_cast<Okular::Document*>(currentDocument);
            for (int page = 0; page < m_okularPages.size(); ++page)
                mutableDocument->refreshPixmaps(page);
        },
        Qt::QueuedConnection);
    return SwapBackingFileReloadInternalData;
}

// Updates OCR scheduling from the pages currently visible in Okular.
void Main::observeOcrFocus()
{
    if (m_placeholder.isActive())
        return;
    if (m_document.type == Model::DocumentType::Epub)
        return;
    const Okular::Document* doc = document();
    const Config::OcrSettings ocrSettings = Config::readOcrSettings();
    if (!ocrSettings.asynchronous || !doc)
        return;
    QList<Plugin::OCR::VisiblePage> visiblePages;
    for (const Okular::VisiblePageRect* visible : doc->visiblePageRects()) {
        const Okular::Page* page = visible ? doc->page(visible->pageNumber) : nullptr;
        if (!page)
            continue;
        const double width = visible->rect.right - visible->rect.left;
        const double height = visible->rect.bottom - visible->rect.top;
        visiblePages.append({ visible->pageNumber, width * height * page->width() * page->height() });
    }
    const Config::OcrTarget target = Config::ocrTargetFor(m_document.hash, ocrSettings);
    m_ocrController->observeVisiblePages(
        visiblePages,
        Config::ocrConfigFor(
            target, static_cast<int>(m_okularPages.size()), dpi().width(), dpi().height(), ocrSettings));
}

bool Main::reopenWorkerDocument()
{
    if (m_okularPages.isEmpty() || (m_document.sourcePath.isEmpty() && m_document.sourceData.isEmpty()))
        return false;

    QList<Plugin::WorkerClient::PageInfo> pages;
    refreshPaperColor();
    if (!m_worker.setSettings(m_settings.documentSettings(m_paperColorRgb))) {
        MU_LOG(warning, "Mu::Generator::Main", "failed to apply settings after worker restart");
        return false;
    }
    const auto openStatus = m_document.sourcePath.isEmpty()
        ? m_worker.openData(m_document.sourceData, m_document.password, pages, m_document.type)
        : m_worker.open(m_document.sourcePath, m_document.password, pages, m_document.type);
    if (openStatus != Model::OpenStatus::Success) {
        MU_LOG(warning, "Mu::Generator::Main", "failed to reopen document after worker restart");
        return false;
    }

    // Reopening is safe only when page count and document identity still match
    // the Okular pages that remain on screen.
    const auto metadata = m_worker.getDocumentInfo({ QStringLiteral("hash") });
    const auto hash = metadata.values.find("hash");
    const bool valid = pages.size() == m_okularPages.size() && metadata.pageCount == m_okularPages.size()
        && (m_document.hash.isEmpty()
            || (hash != metadata.values.end() && QString::fromStdString(hash->second) == m_document.hash));
    if (!valid) {
        MU_LOG(warning, "Mu::Generator::Main", "reopened document does not match the active document");
        m_worker.close();
        return false;
    }

    m_ocrController->reset();
    MU_LOG(warning, "Mu::Generator::Main", "reopened document after worker restart");
    return true;
}

void Main::failClosed(const QString& message)
{
    // Terminal state is sticky; repeated worker failures must not re-notify.
    if (m_placeholder.isActive() && m_placeholder.reason() == Placeholder::Reason::WorkerUnavailable)
        return;

    m_placeholder.activate(message, Placeholder::Reason::WorkerUnavailable);
    clearPlaceholderDerivedState();
    Q_EMIT error(message, 0);

    const Okular::Document* currentDocument = document();
    if (!currentDocument)
        return;
    const int page = static_cast<int>(currentDocument->currentPage());
    if (page >= 0 && page < m_okularPages.size()) {
        clearPageDisplayState(page);
        const_cast<Okular::Document*>(currentDocument)->refreshPixmaps(page);
    }
}

void Main::clearPageDisplayState(int page)
{
    if (page < 0 || page >= m_okularPages.size())
        return;
    const Okular::Document* currentDocument = document();
    if (!currentDocument)
        return;

    const_cast<Okular::Document*>(currentDocument)
        ->setPageTextSelection(page, std::unique_ptr<Okular::RegularAreaRect> { }, QColor());
    Okular::Page* okularPage = m_okularPages.at(page);
    if (!okularPage)
        return;
    okularPage->setTextPage(nullptr);
    okularPage->deletePixmaps();
}

// Okular Generator Func: clears the current document and worker state.
bool Main::doCloseDocument()
{
    // Step 1: Make queued OCR completions harmless before releasing page state.
    m_ocrController->reset();
    QCoreApplication::removePostedEvents(this);
    m_placeholder.reset();
    // Step 2: Clear form proxies and cached UI objects while Okular's user
    // mutex protects their ownership.
    m_formsDirty = false;
    QMutexLocker locker(userMutex());
    clearWorkerDerivedState();
    m_okularPages.clear();

    // Step 3: Reset local identity before asking the worker to discard its copy.
    m_document = { };
    // Closing is best effort: an unavailable worker has already lost its copy.
    if (m_worker.isConnected())
        m_worker.close();

    return true;
}

// Releases the cached Okular embedded-file objects.
void Main::clearEmbeddedFilesCache()
{
    if (!m_embeddedFilesCache)
        return;
    qDeleteAll(*m_embeddedFilesCache);
    m_embeddedFilesCache.reset();
}

// Okular Generator Func: returns the requested document metadata.
Okular::DocumentInfo Main::generateDocumentInfo(const QSet<Okular::DocumentInfo::Key>& keys) const
{
    if (m_placeholder.isActive())
        return { };
    // Native text extraction is worker-owned.
    if (m_worker.isConnected()) {
        const auto info = m_worker.getDocumentInfo();
        if (!info.values.empty()) {
            Okular::DocumentInfo di;
            const QString mimeString = (m_document.type == Model::DocumentType::Epub)
                ? QStringLiteral("application/epub+zip")
                : QStringLiteral("application/pdf");
            di.set(Okular::DocumentInfo::MimeType, mimeString);
            di.set(Okular::DocumentInfo::Pages, QString::number(info.pageCount));

            const auto metadata = [&info](const char* key) {
                const auto it = info.values.find(key);
                return it == info.values.end() ? QString() : QString::fromStdString(it->second);
            };
            const auto setIfRequested = [&](Okular::DocumentInfo::Key key, const char* field) {
                if (!keys.contains(key))
                    return;
                const QString value = metadata(field);
                if (!value.isEmpty())
                    di.set(key, value);
            };

            // Dates arrive as ISO 8601 UTC instants from the worker; display
            // parity with the poppler generator means converting to the local
            // zone and rendering with the locale's long format. Absent or
            // malformed values are skipped.
            const auto setDateTimeIfRequested = [&](Okular::DocumentInfo::Key key, const char* field) {
                if (!keys.contains(key))
                    return;
                const QString raw = metadata(field);
                if (raw.isEmpty())
                    return;
                const QDateTime date = QDateTime::fromString(raw, Qt::ISODate);
                if (!date.isValid())
                    return;
                di.set(key, QLocale().toString(date.toLocalTime(), QLocale::LongFormat));
            };

            setIfRequested(Okular::DocumentInfo::Title, "title");
            setIfRequested(Okular::DocumentInfo::Subject, "subject");
            setIfRequested(Okular::DocumentInfo::Author, "author");
            setIfRequested(Okular::DocumentInfo::Keywords, "keywords");
            setIfRequested(Okular::DocumentInfo::Creator, "creator");
            setIfRequested(Okular::DocumentInfo::Producer, "producer");

            setDateTimeIfRequested(Okular::DocumentInfo::CreationDate, "creationDate");
            setDateTimeIfRequested(Okular::DocumentInfo::ModificationDate, "modificationDate");

            if (keys.contains(Okular::DocumentInfo::CustomKeys)) {
                const QString fmt = metadata("format");
                if (!fmt.isEmpty())
                    di.set(QStringLiteral("format"), fmt, i18n("Format"));
                const QString sec = metadata("security");
                if (!sec.isEmpty())
                    di.set(QStringLiteral("security"), sec, i18n("Security"));
                di.set(QStringLiteral("linearized"),
                       metadata("linearized") == QStringLiteral("true") ? i18n("Yes") : i18n("No"),
                       i18n("Optimized"));
                const int sigs = metadata("signatureCount").toInt();
                di.set(QStringLiteral("signatures"),
                       sigs > 0 ? i18np("Signed (%1 signature)", "Signed (%1 signatures)", sigs) : i18n("Not Signed"),
                       i18n("Digital Signatures"));
            }
            return di;
        }
        MU_LOG(warning, "Mu::Generator::Main", "Worker getDocumentInfo failed");
    }
    return { };
}

// Okular Generator Func: returns the document outline from the worker.
const Okular::DocumentSynopsis* Main::generateDocumentSynopsis()
{
    QMutexLocker locker(userMutex());

    if (m_synopsis) {
        return m_synopsis.get();
    }

    if (m_placeholder.isActive())
        return nullptr;

    std::vector<Model::OutlineNode> nodes;
    if (m_worker.isConnected()) {
        nodes = m_worker.synopsis();
    }
    m_synopsis = Conversion::documentSynopsis(nodes);
    return m_synopsis.get();
}

// Okular Generator Func: returns the fonts used on a page.
Okular::FontInfo::List Main::fontsForPage(int page)
{
    QMutexLocker locker(userMutex());
    if (m_placeholder.isActive())
        return { };
    if (m_worker.isConnected()) {
        const std::vector<Model::Font> source = m_worker.fonts(page);
        Okular::FontInfo::List result;
        for (const auto& font : source) {
            result.append(Conversion::fromModel(font));
        }
        return result;
    }
    return { };
}

// Okular Generator Func: renders a page or tile for Okular.
QImage Main::image(Okular::PixmapRequest* request)
{
    if (request->shouldAbortRender())
        return { };

    const int pageNum = request->page()->number();

    if (m_placeholder.isActive())
        return m_placeholder.image(request->width(), request->height());

    // Rendering is isolated in the worker process.
    if (m_worker.isConnected()) {
        QRect tile;
        if (request->isTile()) {
            // Use 64-bit intermediates before clamping: malformed or rounded
            // normalized rectangles must not overflow the pixel coordinates.
            const QRect requested = request->normalizedRect().geometry(request->width(), request->height());
            const qint64 rawLeft = requested.x();
            const qint64 rawTop = requested.y();
            const qint64 rawRight = rawLeft + requested.width();
            const qint64 rawBottom = rawTop + requested.height();
            const qint64 width = request->width();
            const qint64 height = request->height();
            const qint64 left = qBound<qint64>(0, rawLeft, width);
            const qint64 top = qBound<qint64>(0, rawTop, height);
            const qint64 right = qBound<qint64>(left, rawRight, width);
            const qint64 bottom = qBound<qint64>(top, rawBottom, height);
            if (right <= left || bottom <= top)
                return { };
            tile = QRect(static_cast<int>(left),
                         static_cast<int>(top),
                         static_cast<int>(right - left),
                         static_cast<int>(bottom - top));
        }
        QImage img = m_worker.render(pageNum, request->width(), request->height(), tile);
        if (!img.isNull())
            return img;
        MU_LOG(warning, "Mu::Generator::Main", std::string("Worker render failed for page ") + std::to_string(pageNum));
    }
    return { };
}

// Okular Generator Func: extracts native text or starts/reads OCR.
Okular::TextPage* Main::textPage(Okular::TextRequest* request)
{
    if (request->shouldAbortExtraction())
        return nullptr;
    if (m_placeholder.isActive())
        return nullptr;
    const int pageNum = request->page()->number();

    if (m_document.type == Model::DocumentType::Epub) {
        if (!m_worker.isConnected())
            return nullptr;
        const std::vector<Model::TextBox> workerBoxes =
            m_worker.getTextBoxesForPage(pageNum, dpi().width(), dpi().height(), /*skipAnnots=*/true);
        return Conversion::textPage(workerBoxes, request->page()->width(), request->page()->height());
    }

    // Async OCR emits a TextPage, but the controller retains the result until
    // this request consumes it for hosts that do not immediately handle that
    // signal.
    if (const auto ready = m_ocrController->takeReady(pageNum))
        return Conversion::ocrTextPage(*ready);
    if (m_worker.isConnected()) {
        const std::vector<Model::TextBox> workerBoxes =
            m_worker.getTextBoxesForPage(pageNum, dpi().width(), dpi().height(), /*skipAnnots=*/true);
#ifdef MUPDF_HAS_OCR
        const Config::OcrSettings ocrSettings = Config::readOcrSettings();
        const Config::OcrTarget ocrTarget = Config::ocrTargetFor(m_document.hash, ocrSettings);
        const auto ocrConfig = Config::ocrConfigFor(
            ocrTarget, static_cast<int>(m_okularPages.size()), dpi().width(), dpi().height(), ocrSettings);
        const bool useOcr = Plugin::OCR::Controller::shouldTrigger(
            ocrConfig.force, ocrConfig.autoTrigger, ocrConfig.triggerThreshold, workerBoxes.size());
        if (ocrSettings.asynchronous)
            QMetaObject::invokeMethod(this, [this] { observeOcrFocus(); }, Qt::QueuedConnection);
        if (useOcr) {
            const auto key =
                Plugin::Caching::OCR::Cache::normalizeKey(ocrTarget.documentHash, ocrTarget.language, ocrTarget.dpi);
            if (!key)
                return nullptr;
            const auto cached = Plugin::Caching::OCR::Cache::load(*key, pageNum);
            if (cached.present)
                return Conversion::ocrTextPage(cached.items);
            if (ocrSettings.asynchronous) {
                return nullptr;
            }
            const Model::OcrResult ocrResult = m_worker.ocrPage(pageNum, key->language, key->dpi, false);
            if (ocrResult.status != Model::OcrStatus::Success)
                return nullptr;
            const QVector<Plugin::Caching::OCR::CacheItem> items =
                Plugin::Caching::OCR::Cache::convertToCacheItems(ocrResult.boxes);
            Plugin::Caching::OCR::Cache::save(*key, pageNum, items);
            return Conversion::ocrTextPage(items);
        }
#endif
        return Conversion::textPage(workerBoxes, request->page()->width(), request->page()->height());
    }
    return nullptr;
}

// Okular Generator Func: returns generator-specific metadata.
QVariant Main::metaData(const QString& key, const QVariant& option) const
{
    Q_UNUSED(option)

    if (key == QLatin1String("DocumentTitle")) {
        return m_document.name;
    }
    if (key == QLatin1String("GeneratorExtraDescription")) {
        return generatorExtraDescription();
    }

    return QVariant();
}

// Okular Generator Func: reports pixel metric for page size localization.
Okular::Generator::PageSizeMetric Main::pagesSizeMetric() const
{
    return Pixels;
}

// Okular Generator Func: returns embedded files as Okular objects.
const QList<Okular::EmbeddedFile*>* Main::embeddedFiles() const
{
    if (m_document.type == Model::DocumentType::Epub)
        return nullptr;
    if (m_placeholder.isActive())
        return nullptr;
    QMutexLocker locker(userMutex());
    // Convert shared embedded-file models to Okular objects on first
    // call. The generator owns the converted list via m_embeddedFilesCache.
    if (!m_embeddedFilesCache) {
        if (!m_worker.isConnected())
            return nullptr;
        const auto source = m_worker.embeddedFiles();
        m_embeddedFilesCache = std::make_unique<QList<Okular::EmbeddedFile*>>();
        for (const auto& ef : source) {
            m_embeddedFilesCache->append(Conversion::embeddedFile(ef).release());
        }
    }
    return m_embeddedFilesCache.get();
}

// Okular Generator Func: reports the supported save options.
bool Main::supportsOption(SaveOption option) const
{
    if (m_document.type == Model::DocumentType::Epub)
        return false;
    if (m_placeholder.isActive())
        return false;
    return option == SaveChanges;
}

// Okular Generator Func: saves the current document through the worker.
bool Main::save(const QString& fileName, SaveOptions options, QString* errorText)
{
    Q_UNUSED(options)
    if (m_placeholder.isActive()) {
        if (errorText)
            *errorText = m_placeholder.reason() == Placeholder::Reason::SandboxGate
                ? i18n("Strict sandbox enforcement blocks this document.")
                : i18n("MuPDF worker is unavailable.");
        return false;
    }
    if (m_worker.isConnected()) {
        if (m_worker.saveToFile(fileName))
            return true;
        if (errorText)
            *errorText = i18n("Failed to save PDF document.");
        return false;
    }
    if (errorText)
        *errorText = i18n("MuPDF worker is unavailable.");
    return false;
}

// Okular Generator Func: reports the supported export formats.
Okular::ExportFormat::List Main::exportFormats() const
{
    return { Okular::ExportFormat::standardFormat(Okular::ExportFormat::PlainText) };
}

// Okular Generator Func: exports the document text to a file.
bool Main::exportTo(const QString& fileName, const Okular::ExportFormat& format)
{
    if (!format.mimeType().inherits(QStringLiteral("text/plain")) || m_placeholder.isActive()
        || !m_worker.isConnected())
        return false;

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QTextStream stream(&file);
    const int pageCount = document() ? static_cast<int>(document()->pages()) : 0;
    for (int page = 0; page < pageCount; ++page) {
        if (!m_worker.isConnected())
            return false;
        const auto boxes = m_worker.getTextBoxesForPage(page, dpi().width(), dpi().height(), /*skipAnnots=*/true);
        stream << Conversion::plainText(boxes);
        if (page + 1 < pageCount)
            stream << QLatin1Char('\n');
    }
    return true;
}

// Okular Generator Func: returns the annotation adapter used by Okular.
Okular::AnnotationProxy* Main::annotationProxy() const
{
    if (m_document.type == Model::DocumentType::Epub)
        return nullptr;
    return &m_annotationProxy;
}

// Okular Generator Func: prints through a temporary worker output file.
Okular::Document::PrintError Main::print(QPrinter& printer)
{
    if (m_placeholder.isActive() || !m_worker.isConnected())
        return Okular::Document::FileConversionPrintError;

    const int totalPages = !m_okularPages.isEmpty() ? static_cast<int>(m_okularPages.size()) : 0;
    if (totalPages <= 0)
        return Okular::Document::NoFileToPrintError;

    const int curPage = document() ? static_cast<int>(document()->currentPage()) + 1 : 1;
    const QList<int> bookmarked = document() ? document()->bookmarkedPageList() : QList<int>();

    const QList<int> pageList = Okular::FilePrinter::pageList(printer, totalPages, curPage, bookmarked);

    QVector<int> selectedPages;
    selectedPages.reserve(pageList.size());
    for (int p : pageList) {
        if (p >= 1 && p <= totalPages)
            selectedPages.append(p - 1);
    }

    const QString templatePath = Plugin::Util::tempDirectory() + QStringLiteral("/print-XXXXXX.pdf");
    QTemporaryFile tempFile(templatePath);
    if (!tempFile.open())
        return Okular::Document::TemporaryFileOpenPrintError;

    const QString fileName = tempFile.fileName();
    tempFile.setAutoRemove(false);
    tempFile.close();

    if (!m_worker.printPdfToFile(fileName, selectedPages)) {
        QFile::remove(fileName);
        return Okular::Document::FileConversionPrintError;
    }

    QPageLayout::Orientation orientation = QPageLayout::Portrait;
    if (!m_okularPages.isEmpty() && m_okularPages.first()->width() > m_okularPages.first()->height()) {
        orientation = QPageLayout::Landscape;
    } else {
        orientation = printer.pageLayout().orientation();
    }

    const QString bookmarkedRange = document() ? document()->bookmarkedPageRange() : QString();

    // Prevent CUPS / FilePrinter double-filtering when ApplicationSelectsPages is used
    const auto origRange = printer.printRange();
    printer.setPrintRange(QPrinter::AllPages);

    // Scale mode: None keeps the original size; both fit modes rely on the
    // printer's fit-to-page handling (poppler maps them identically).
    const auto scaleMode =
        static_cast<PrintScaleMode>(std::clamp<quint32>(MuPDFSettings::self()->printScaleMode(), 0, 2));
    const auto filePrinterScaleMode = scaleMode == PrintScaleMode::None
        ? Okular::FilePrinter::ScaleMode::NoScaling
        : Okular::FilePrinter::ScaleMode::FitToPrintArea;

    Okular::Document::PrintError err = Okular::FilePrinter::printFile(printer,
                                                                      fileName,
                                                                      orientation,
                                                                      Okular::FilePrinter::SystemDeletesFiles,
                                                                      Okular::FilePrinter::ApplicationSelectsPages,
                                                                      bookmarkedRange,
                                                                      filePrinterScaleMode);
    if (err != Okular::Document::NoPrintError)
        QFile::remove(fileName);

    printer.setPrintRange(origRange);
    return err;
}

// Okular Generator Func: builds the extra print options widget. Scale mode
// changes are persisted immediately so print() can read the current value.
QWidget* Main::printConfigurationWidget() const
{
    auto* page = new PrintOptionsPage(
        static_cast<PrintScaleMode>(std::clamp<quint32>(MuPDFSettings::self()->printScaleMode(), 0, 2)));
    connect(page, &PrintOptionsPage::scaleModeChanged, this, [](PrintScaleMode mode) {
        MuPDFSettings::self()->setPrintScaleMode(static_cast<quint32>(mode));
        MuPDFSettings::self()->save();
    });
    return page;
}

// Okular Generator Func: signs the document using the supplied data.
std::pair<Okular::SigningResult, QString> Main::sign(const Okular::NewSignatureData& data, const QString& rFilename)
{
    if (m_placeholder.isActive())
        return { Okular::GenericSigningError,
                 m_placeholder.reason() == Placeholder::Reason::WorkerUnavailable
                     ? QStringLiteral("MuPDF worker is unavailable")
                     : QStringLiteral("Strict sandbox enforcement blocks this document") };
    if (m_worker.isConnected()) {
        const Okular::NormalizedRect rect = data.boundingRectangle();
        const QString commonName = Plugin::Crypto::signingCertificateCommonName(data.certNickname());
        if (commonName.isEmpty())
            return { Okular::KeyMissing, QStringLiteral("Signing certificate was not found") };
        const auto bgImage = Plugin::Util::SignatureImage::prepareBackgroundImage(
            data.backgroundImagePath(), rect.width(), rect.height());
        const auto result = m_worker.sign({ { },
                                            data.page() >= 0 ? data.page() : 0,
                                            { rect.left, rect.top, rect.right, rect.bottom },
                                            data.certNickname().toStdString(),
                                            commonName.toStdString(),
                                            data.reason().toStdString(),
                                            data.location().toStdString(),
                                            -1,
                                            bgImage },
                                          data.password(),
                                          rFilename);
        switch (result.result) {
        case Model::SigningResult::Success:
            return { Okular::SigningSuccess, QString() };
        case Model::SigningResult::FieldAlreadySigned:
            return { Okular::FieldAlreadySigned, QString::fromStdString(result.details) };
        case Model::SigningResult::KeyMissing:
            return { Okular::KeyMissing, QString::fromStdString(result.details) };
        case Model::SigningResult::WriteFailed:
            return { Okular::SignatureWriteFailed, QString::fromStdString(result.details) };
        case Model::SigningResult::UserCancelled:
            return { Okular::UserCancelled, QString::fromStdString(result.details) };
        case Model::SigningResult::BadPassphrase:
            return { Okular::BadPassphrase, QString::fromStdString(result.details) };
        default:
            return { Okular::GenericSigningError, QString::fromStdString(result.details) };
        }
    }
    return { Okular::GenericSigningError, QStringLiteral("MuPDF worker is unavailable") };
}

// Okular Generator Func: reports that worker-backed signing is supported.
bool Main::canSign() const
{
    if (m_document.type == Model::DocumentType::Epub)
        return false;
    if (m_placeholder.isActive())
        return false;
    return !Plugin::Crypto::signingCertificates().isEmpty();
}

// Okular Generator Func: returns the certificate store used for signing.
Okular::CertificateStore* Main::certificateStore() const
{
    return m_certStore.get();
}

} // namespace Mu::Generator

#include "mupdfng.moc"
