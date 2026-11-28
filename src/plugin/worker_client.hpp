// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_WORKER_CLIENT_HPP
#define MU_PLUGIN_WORKER_CLIENT_HPP

#include <QImage>
#include <QObject>
#include <QStringList>

#include <functional>

#include "paperbox_model.hpp"
#include "shared/form_backend.hpp"
#include <paperbox/client.hpp>

namespace Mu::Plugin {

// Qt adapter for Paperbox operations and events.
class WorkerClient final : public QObject, public FormBackend {
    Q_OBJECT

public:
    /// Worker process/session lifecycle. The process can be running while the
    /// generator has not yet confirmed its document session (Recovering); user
    /// operations must be gated on State::Ready.
    using State = ::paperbox::State;

    using PageInfo = Model::PageInfo;
    explicit WorkerClient(QObject* parent = nullptr);
    ~WorkerClient() override;
    bool start(const QString& binaryPath, const QStringList& tessDataDirectories = { });
    void stop();

    /// Use Paperbox directly for document operations with native types.
    [[nodiscard]] const ::paperbox::WorkerClient& paperboxClient() const noexcept { return m_client; }

    [[nodiscard]] bool operational() const noexcept { return m_client.state() == State::Ready; }

    /// Marks recovery complete after the generator revalidated its document.
    void commitSessionReady();
    /// Marks recovery unrecoverable; no automatic restart will make it Ready.
    void commitSessionFailed();
    Model::OpenStatus open(const QString& path,
                           const QString& password,
                           QList<PageInfo>& pages,
                           Model::DocumentType type = Model::DocumentType::Pdf);
    Model::OpenStatus openData(const QByteArray& data,
                               const QString& password,
                               QList<PageInfo>& pages,
                               Model::DocumentType type = Model::DocumentType::Pdf);
    void validateSignatures(QList<PageInfo>& pages, bool checkOcsp) const;
    /// Cancellation is checked on the calling thread and sets the active
    /// render's shared MuPDF abort flag. The terminal response is still drained
    /// on the transport thread, including any frame produced before cancellation.
    QImage render(int page,
                  int width,
                  int height,
                  const QRect& tile = { },
                  const std::function<bool()>& shouldAbort = { },
                  Model::RenderLinks* links = nullptr);
    Model::ResolvedLink resolveLink(const QString& uri) const;
    Model::DocumentMetadata getDocumentInfo(const QStringList& keys = { }) const;
    std::optional<quint64> startOcrPage(int page, const QString& language, int dpi) const;
    bool exportDocument(const QString& target, const Model::ExportOptions& options);
    /// Returns immediately; completion arrives via exportProgress.
    std::optional<quint64> startExport(const QString& target, const Model::ExportOptions& options) const;
    Model::SignResponse sign(const Model::SignOptions& request, const QString& password, const QString& target);
    bool
    modifyAnnotation(int page, const QString& id, const Model::Annotation& annotation, bool appearanceChanged) const;
    bool removeAnnotation(int page, const QString& id) const;
    std::optional<Model::FormUpdateResponse> updateForm(const Model::FormUpdateRequest& request) const override;
    std::optional<Model::FormUpdateResponse> resetForm(const Model::FormResetRequest& request) const override;
    std::optional<Model::FormUpdateResponse>
    clickFormButton(const Model::FormButtonClickRequest& request) const override;

signals:
    void operationFailed(const QString& message, Model::ErrorCode code);
    void workerDied(int exitCode);
    /// A fresh worker is available. It deliberately has no document open.
    void workerRestarted();
    /// Automatic recovery is disabled after repeated worker failures.
    void workerUnavailable();
    void ocrDone(quint64 jobId, int page);
    void pageLinksReady(quint64 epoch, std::vector<Model::PageLinks> pages, bool resourceLimited, const QString& error);
    void exportProgress(::paperbox::signal::ExportProgressEvent progress);

private:
    void deliverEvents();
    ::paperbox::WorkerClient m_client;
};

} // namespace Mu::Plugin

#endif // MU_PLUGIN_WORKER_CLIENT_HPP
