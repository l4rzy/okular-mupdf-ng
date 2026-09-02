// SPDX-FileCopyrightText: 2008 Pino Toscano <pino@kde.org>
// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef GENERATOR_MUPDF_H
#define GENERATOR_MUPDF_H

#include <okular/core/action.h>
#include <okular/core/area.h>
#include <okular/core/document.h>
#include <okular/core/fontinfo.h>
#include <okular/core/generator.h>
#include <okular/core/sourcereference.h>
#include <okular/core/version.h>
#include <okular/interfaces/configinterface.h>
#include <okular/interfaces/saveinterface.h>

#include <QByteArray>
#include <memory>

#include "generator/config/settings.hpp"
#include "generator/placeholder.hpp"
#include "generator/proxy/annotation.hpp"
#include "generator/proxy/certificate_store.hpp"
#include "generator/proxy/form/coordinator.hpp"
#include "generator/proxy/form/signature.hpp"
#include "plugin/ocr/ocr.hpp"
#include "plugin/worker_client.hpp"

namespace Mu::Generator {

/// Okular-facing coordinator for the isolated MuPDF worker.
///
/// Lifecycle overview:
/// 1. Open a document through WorkerClient and translate its models into Okular pages.
/// 2. Forward rendering, text, form, save, print, and signing requests to the worker.
/// 3. Keep the source and UI state needed to recover after a worker restart.
/// 4. Cancel asynchronous OCR work before document teardown or worker recovery.
class Main : public Okular::Generator, public Okular::SaveInterface, public Okular::ConfigInterface {
    Q_OBJECT
    Q_INTERFACES(Okular::Generator)
    Q_INTERFACES(Okular::SaveInterface)
    Q_INTERFACES(Okular::ConfigInterface)

public:
    // Okular Generator Func: creates the generator and initializes worker services.
    Main(QObject* parent, const QVariantList& args);
    // Okular Generator Func: stops worker activity and releases generator resources.
    virtual ~Main();

    // Okular Generator Func: opens a file and creates its Okular pages.
    Okular::Document::OpenResult
    loadDocumentWithPassword(const QString& fileName, QVector<Okular::Page*>& pages, const QString& password) override;
    // Okular Generator Func: opens memory data and creates its Okular pages.
    Okular::Document::OpenResult loadDocumentFromDataWithPassword(const QByteArray& fileData,
                                                                  QVector<Okular::Page*>& pages,
                                                                  const QString& password) override;
    // Okular Generator Func: replaces the backing file while preserving page state.
    SwapBackingFileResult swapBackingFile(const QString& newFileName, QList<Okular::Page*>& newPages) override;

    // Okular Generator Func: returns the requested document metadata.
    Okular::DocumentInfo generateDocumentInfo(const QSet<Okular::DocumentInfo::Key>& keys) const override;
    // Okular Generator Func: returns the document outline from the worker.
    const Okular::DocumentSynopsis* generateDocumentSynopsis() override;
    // Okular Generator Func: returns the fonts used on a page.
    Okular::FontInfo::List fontsForPage(int page) override;
    // Okular Generator Func: returns generator-specific metadata.
    QVariant metaData(const QString& key, const QVariant& option) const override;
    // Okular Generator Func: reports pixel metric for page size localization.
    PageSizeMetric pagesSizeMetric() const override;

    // Okular Generator Func: returns embedded files as Okular objects.
    const QList<Okular::EmbeddedFile*>* embeddedFiles() const override;
    // Okular Generator Func: reports the supported save options.
    bool supportsOption(SaveOption option) const override;
    // Okular Generator Func: saves the current document through the worker.
    bool save(const QString& fileName, SaveOptions options, QString* errorText) override;
    // Okular Generator Func: reports the supported export formats.
    Okular::ExportFormat::List exportFormats() const override;
    // Okular Generator Func: exports the document text to a file.
    bool exportTo(const QString& fileName, const Okular::ExportFormat& format) override;
    // Okular Generator Func: returns the annotation adapter used by Okular.
    Okular::AnnotationProxy* annotationProxy() const override;

    // Okular Generator Func: reports that worker-backed signing is supported.
    bool canSign() const override;
    // Okular Generator Func: signs the document using the supplied data.
    std::pair<Okular::SigningResult, QString> sign(const Okular::NewSignatureData& data,
                                                   const QString& rFilename) override;
    // Okular Generator Func: returns the certificate store used for signing.
    Okular::CertificateStore* certificateStore() const override;

protected:
    // Okular Generator Func: clears the current document and worker state.
    bool doCloseDocument() override;
    // Okular Generator Func: renders a page or tile for Okular.
    QImage image(Okular::PixmapRequest* page) override;
    // Okular Generator Func: extracts native text or starts/reads OCR.
    Okular::TextPage* textPage(Okular::TextRequest* request) override;
    // Okular Generator Func: prints through a temporary worker output file.
    Okular::Document::PrintError print(QPrinter& printer) override;

    // Okular Generator Func: reloads settings and reports rendering changes.
    bool reparseConfig() override;
    // Okular Generator Func: adds the generator settings page to Okular.
    void addPages(KConfigDialog* dialog) override;

private:
    // Updates OCR scheduling from the pages currently visible in Okular.
    void observeOcrFocus();
    // Reopens the retained source after a worker restart and verifies that it
    // still represents the active Okular document.
    bool reopenWorkerDocument();
    // Permanently disables the active document after an unrecoverable worker failure.
    void failClosed(const QString& message);
    // Clears transient Okular display state without removing document data.
    void clearPageDisplayState(int page);
    // Tracks settings that are fixed when the generator process starts.
    // Refreshes restart-required state: fresh EPUB settings are compared
    // against the frozen startup set; the NSS database is the runtime-checked
    // half — initialization is one-way per process, so a changed database
    // path only applies after an Okular restart.
    void updateRestartRequiredSettings();
    // Reports whether Strict enforcement currently blocks the not-fully-hardened worker.
    [[nodiscard]] bool sandboxGated() const;
    // Loads a single synthetic placeholder page while Strict enforcement
    // withholds the real document from the worker.
    Okular::Document::OpenResult loadBlockedPlaceholderDocument(QVector<Okular::Page*>& pages);
    // Drops worker-derived UI state while the placeholder withholds the document.
    void clearPlaceholderDerivedState();
    // Must be called while userMutex() is held; drops cached UI objects
    // derived from the worker document.
    void clearWorkerDerivedState();
    // Queues an Okular close/open cycle so placeholder deactivation restores
    // the real document after Strict enforcement relaxes.
    void reopenWithheldDocument();
    // Executes the queued cycle; maps the module's outcome to user signals.
    void reopenWithheldDocumentInternal();

    // Converts worker page information into Okular-owned pages and wires their
    // annotations, links, and form proxies to the current worker session.
    Okular::Document::OpenResult
    initPages(QVector<Okular::Page*>& pages, QList<Plugin::WorkerClient::PageInfo>& pageInfos, const QString& password);
    // Must be called while userMutex() is held; releases cached embedded files.
    void clearEmbeddedFilesCache();

    // -----------------------------------------------------------------------
    // Trusted plugin facade for all worker operations. It owns the IPC client
    // and keeps MuPDF outside the Okular process.
    // -----------------------------------------------------------------------
    Plugin::WorkerClient m_worker;
    // Owns asynchronous OCR scheduling, focus tracking, and result retention;
    // Okular-facing glue (TextPage building, notifications) lives here.
    std::unique_ptr<Plugin::OCR::Controller> m_ocrController;

    std::unique_ptr<Okular::DocumentSynopsis> m_synopsis;
    mutable std::unique_ptr<QList<Okular::EmbeddedFile*>> m_embeddedFilesCache;
    mutable Proxy::Annotation m_annotationProxy;

    // Identity and retained source of the currently open document; reset as a
    // whole on close and rebuilt by every load path.
    struct Document {
        QString password;
        QString name;
        // Exactly one source is retained while a document is open so a
        // restarted worker can reopen the same document without asking Okular
        // for it again.
        QString sourcePath;
        QByteArray sourceData;
        QString hash;
        Model::DocumentType type = Model::DocumentType::Pdf;
    };

    Document m_document;

    QVector<Okular::Page*> m_okularPages;
    std::unique_ptr<Proxy::CertificateStore> m_certStore;
    std::unique_ptr<Proxy::Form::Coordinator> m_formCoordinator;
    bool m_formsDirty = false;

    // Single owner of interrupted-display state: sandbox-gate withholding
    // (restorable through an Okular reopen) and unrecoverable worker failure
    // (terminal for the generator lifetime).
    Placeholder m_placeholder;

    // Session-scope worker configuration; EPUB settings are fixed during
    // worker startup (changes require restarting Okular rather than being
    // sent to the already-sandboxed worker).
    Config::WorkerSettings m_settings;

    // Restart decision state: whether pending settings need an Okular
    // restart, and whether the one-shot dialog notice was already shown.
    struct RestartState {
        bool required = false;
        bool noticeShown = false;
    };

    RestartState m_restartState;
};

} // namespace Mu::Generator

#endif
