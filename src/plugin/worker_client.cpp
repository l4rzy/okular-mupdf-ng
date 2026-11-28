// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/worker_client.hpp"
#include "plugin/caching/cache_root.hpp"
#include <paperbox/logging.hpp>
#include <paperbox/nss.hpp>

#include <QFile>
#include <QFileInfo>
#include <QRect>
#include <QStandardPaths>
#include <chrono>
#include <iterator>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace Mu::Plugin {
namespace {

std::filesystem::path nativePath(const QString& path)
{
    return QFile::encodeName(path).toStdString();
}

} // namespace

WorkerClient::WorkerClient(QObject* parent)
    : QObject(parent)
{
    m_client.setEventWakeup(
        [this] { QMetaObject::invokeMethod(this, &WorkerClient::deliverEvents, Qt::QueuedConnection); });
}

WorkerClient::~WorkerClient()
{
    m_client.setEventWakeup({ });
    m_client.stop();
}

bool WorkerClient::start(const QString& binaryPath, const QStringList& tessDataDirectories)
{
    ::paperbox::LaunchOptions options;
    QString path = binaryPath;
    if (path.isEmpty())
        path = qEnvironmentVariable("PAPERBOX_WORKER");
#ifdef WORKER_BUILD_PATH
    if (path.isEmpty() && QFileInfo(QStringLiteral(WORKER_BUILD_PATH)).isExecutable())
        path = QStringLiteral(WORKER_BUILD_PATH);
#endif // WORKER_BUILD_PATH
    if (path.isEmpty() && QFileInfo(QStringLiteral(WORKER_INSTALL_PATH)).isExecutable())
        path = QStringLiteral(WORKER_INSTALL_PATH);
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(QStringLiteral("paperbox-worker"));
    options.binary = nativePath(path);
    for (const auto& directory : tessDataDirectories)
        options.tessDataDirectories.push_back(nativePath(directory));
    options.cacheRoot = Caching::cacheRoot();
    const auto profile = qgetenv("MUPDFNG_WORKER_SANDBOX");
    if (profile == "bwrap") {
        const auto launcher = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
        if (launcher.isEmpty())
            return false;
        options.bubblewrap = nativePath(launcher);
    } else if (!profile.isEmpty() && profile != "builtin") {
        paperbox::log::write(paperbox::log::Level::Warning,
                             "Mu::Plugin",
                             "Unknown MUPDFNG_WORKER_SANDBOX value. Falling back to the built-in sandbox.");
    }
    return m_client.start(std::move(options));
}

void WorkerClient::deliverEvents()
{
    for (auto& event : m_client.takeEvents()) {
        std::visit(
            [this](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ::paperbox::signal::OperationFailedEvent>)
                    Q_EMIT operationFailed(QString::fromStdString(value.message), value.code);
                else if constexpr (std::is_same_v<T, ::paperbox::signal::WorkerDiedEvent>)
                    Q_EMIT workerDied(value.exitCode);
                else if constexpr (std::is_same_v<T, ::paperbox::signal::WorkerRestartedEvent>)
                    Q_EMIT workerRestarted();
                else if constexpr (std::is_same_v<T, ::paperbox::signal::WorkerUnavailableEvent>)
                    Q_EMIT workerUnavailable();
                else if constexpr (std::is_same_v<T, ::paperbox::signal::OcrDoneEvent>)
                    Q_EMIT ocrDone(value.jobId, value.page);
                else if constexpr (std::is_same_v<T, ::paperbox::signal::PageLinksReadyEvent>)
                    Q_EMIT pageLinksReady(value.epoch,
                                          std::move(value.pages),
                                          value.resourceLimited,
                                          QString::fromStdString(value.error));
                else if constexpr (std::is_same_v<T, ::paperbox::signal::ExportProgressEvent>)
                    Q_EMIT exportProgress(std::move(value));
            },
            std::move(event));
    }
}

Model::OpenStatus
WorkerClient::open(const QString& path, const QString& password, QList<PageInfo>& pages, Model::DocumentType type)
{
    std::vector<PageInfo> native;
    const auto result = m_client.open(nativePath(path), password.toStdString(), &native, type);
    if (result == Model::OpenStatus::Success)
        pages.assign(std::make_move_iterator(native.begin()), std::make_move_iterator(native.end()));
    return result;
}

Model::OpenStatus WorkerClient::openData(const QByteArray& data,
                                         const QString& password,
                                         QList<PageInfo>& pages,
                                         Model::DocumentType type)
{
    std::vector<PageInfo> native;
    const auto bytes = std::as_bytes(std::span(data.constData(), static_cast<std::size_t>(data.size())));
    const auto result = m_client.openData(bytes, password.toStdString(), &native, type);
    if (result == Model::OpenStatus::Success)
        pages.assign(std::make_move_iterator(native.begin()), std::make_move_iterator(native.end()));
    return result;
}

void WorkerClient::validateSignatures(QList<PageInfo>& pages, bool checkOcsp) const
{
    paperbox::nss::validateDocumentSignatures(
        m_client, { pages.data(), static_cast<std::size_t>(pages.size()) }, checkOcsp);
}

QImage WorkerClient::render(int page,
                            int width,
                            int height,
                            const QRect& tile,
                            const std::function<bool()>& shouldAbort,
                            Model::RenderLinks* links)
{
    if (links)
        *links = { };
    std::optional<Model::RenderTile> viewport;
    if (!tile.isNull())
        viewport = Model::RenderTile { tile.x(), tile.y(), tile.width(), tile.height() };
    if (shouldAbort && shouldAbort())
        return { };
    auto job = m_client.renderAsync(page, width, height, viewport, links != nullptr);
    bool ready;
    do {
        ready = job.waitFor(std::chrono::milliseconds(10));
        if (shouldAbort && shouldAbort()) {
            job.cancel();
            return { };
        }
    } while (!ready);
    auto result = job.take();
    if (links)
        *links = std::move(result.links);
    if (!result.frame)
        return { };
    auto frame = std::make_unique<::paperbox::RenderedFrame>(std::move(result.frame));
    const auto cleanup = [](void* pointer) {
        delete static_cast<::paperbox::RenderedFrame*>(pointer);
    };
    QImage image(reinterpret_cast<const uchar*>(frame->pixels().data()),
                 frame->width(),
                 frame->height(),
                 frame->stride(),
                 QImage::Format_RGBA8888,
                 cleanup,
                 frame.get());
    if (!image.isNull())
        (void)frame.release();
    return image;
}

Model::DocumentMetadata WorkerClient::getDocumentInfo(const QStringList& keys) const
{
    std::vector<std::string> native;
    native.reserve(static_cast<std::size_t>(keys.size()));
    for (const auto& key : keys)
        native.push_back(key.toStdString());
    return m_client.getDocumentInfo(native);
}

Model::SignResponse
WorkerClient::sign(const Model::SignOptions& options, const QString& password, const QString& target)
{
    auto bytes = password.toUtf8();
    auto provider = paperbox::nss::makeSigningProvider({ bytes.constData(), static_cast<std::size_t>(bytes.size()) });
    bytes.fill('\0');
    return m_client.signToFile(options, std::move(provider), nativePath(target));
}

bool WorkerClient::exportDocument(const QString& target, const Model::ExportOptions& options)
{
    return m_client.exportDocument(nativePath(target), options);
}

std::optional<quint64> WorkerClient::startExport(const QString& target, const Model::ExportOptions& options) const
{
    return m_client.startExport(nativePath(target), options);
}

Model::ResolvedLink WorkerClient::resolveLink(const QString& uri) const
{
    return m_client.resolveLink(uri.toStdString());
}

std::optional<quint64> WorkerClient::startOcrPage(int page, const QString& language, int dpi) const
{
    return m_client.startOcrPage(page, language.toStdString(), dpi);
}

bool WorkerClient::modifyAnnotation(int page, const QString& id, const Model::Annotation& value, bool appearance) const
{
    return m_client.modifyAnnotation(page, id.toStdString(), value, appearance);
}

bool WorkerClient::removeAnnotation(int page, const QString& id) const
{
    return m_client.removeAnnotation(page, id.toStdString());
}

void WorkerClient::stop()
{
    m_client.stop();
}

void WorkerClient::commitSessionReady()
{
    m_client.commitSessionReady();
}

void WorkerClient::commitSessionFailed()
{
    m_client.commitSessionFailed();
}

std::optional<Model::FormUpdateResponse> WorkerClient::updateForm(const Model::FormUpdateRequest& request) const
{
    return m_client.updateForm(request);
}

std::optional<Model::FormUpdateResponse> WorkerClient::resetForm(const Model::FormResetRequest& request) const
{
    return m_client.resetForm(request);
}

std::optional<Model::FormUpdateResponse>
WorkerClient::clickFormButton(const Model::FormButtonClickRequest& request) const
{
    return m_client.clickFormButton(request);
}

} // namespace Mu::Plugin
