// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_transport.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QUuid>
#ifdef MU_DEBUG_ENABLED
#include <chrono>
#endif
#include <optional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "plugin/caching/epub_cache.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/util/temp_dir.hpp"
#include "shared/logging.hpp"
#include "shared/protocol/ipc_debug.hpp"
#include "shared/protocol/zpp_codec.hpp"
#include "shared/transport/compat.hpp"
#include "shared/transport/fd_channel.hpp"
#include "shared/transport/frame_buffer.hpp"

namespace Mu::Plugin {

using namespace ::Mu::Model;
namespace Timeout = ::Mu::IPC::Timeout;

namespace {

struct MappedFrame {
    void* mapping = nullptr;
    std::size_t size = 0;
};

void cleanupMappedFrame(void* data)
{
    auto* frame = static_cast<MappedFrame*>(data);
    if (!frame)
        return;
    if (frame->mapping && frame->mapping != MAP_FAILED)
        ::munmap(frame->mapping, frame->size);
    delete frame;
}

int timeoutFor(const RequestPayload& payload)
{
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RenderRequest>)
                return Timeout::RenderMs;
            if constexpr (std::is_same_v<T, OcrPageRequest> || std::is_same_v<T, OcrResultRequest>)
                return Timeout::OcrMs;
            if constexpr (std::is_same_v<T, SignRequest>)
                return Timeout::SignMs;
            return Timeout::GenericOpMs;
        },
        payload);
}

} // namespace

struct WorkerTransport::FrameSlotMapping {
    void* mapping = MAP_FAILED;
    std::size_t size = 0;

    ~FrameSlotMapping()
    {
        if (mapping != MAP_FAILED)
            ::munmap(mapping, size);
    }
};

struct WorkerTransport::FrameLease {
    QPointer<WorkerTransport> transport;
    quint64 session = 0;
    quint64 slotId = 0;
    quint64 leaseId = 0;
    std::shared_ptr<FrameSlotMapping> mapping;
};

void WorkerTransport::cleanupFrameLease(void* data)
{
    std::unique_ptr<FrameLease> lease(static_cast<FrameLease*>(data));
    const QPointer<WorkerTransport> transport = lease->transport;
    if (!transport)
        return;
    QMetaObject::invokeMethod(
        transport.data(),
        [transport, session = lease->session, slotId = lease->slotId, leaseId = lease->leaseId] {
            if (transport)
                transport->releaseFrameSlot(session, slotId, leaseId);
        },
        Qt::QueuedConnection);
}

void WorkerTransport::releaseFrameSlot(quint64 session, quint64 slotId, quint64 leaseId)
{
    if (session != m_frameSession || !isConnected())
        return;
    const auto response = call(ReleaseFrameSlotRequest { slotId, leaseId });
    if (!response || response->error)
        MU_LOG(warning, "Mu::Plugin", "could not release render frame slot");
}

bool WorkerTransport::start(const QString& binaryPath,
                            const QStringList& tessDataDirectories,
                            Model::SandboxStatus* sandboxStatus)
{
    // Start is also the recovery path. Remove every artifact from a partial
    // previous session before allocating new authenticated endpoints.
    if (isConnected())
        stop();
    else
        cleanupSession();
    m_intentionalStop = false;
    const auto failStart = [this] {
        cleanupSession();
        return false;
    };
    Util::cleanupStaleTempFiles();
    const QString resolvedBinaryPath = findBinary(binaryPath);
    if (resolvedBinaryPath.isEmpty())
        return failStart();
    const QString baseUuid = QUuid::createUuid().toString(QUuid::Id128);
    const QString tmpDir = Util::tempDirectory();
    m_socketPath =
        tmpDir + QStringLiteral("/worker-%1-%2-ctrl.sock").arg(QCoreApplication::applicationPid()).arg(baseUuid);
    m_fdSocketPath =
        tmpDir + QStringLiteral("/worker-%1-%2-fd.sock").arg(QCoreApplication::applicationPid()).arg(baseUuid);
    std::string e;
    if (!m_fd.listen(m_fdSocketPath.toStdString(), &e))
        return failStart();
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    QStringList arguments { QStringLiteral("--socket"), m_socketPath, QStringLiteral("--fd-socket"), m_fdSocketPath };
    for (const QString& directory : tessDataDirectories) {
        if (!directory.isEmpty())
            arguments << QStringLiteral("--tessdata-dir") << directory;
    }
    m_process.start(resolvedBinaryPath, arguments);
    if (!m_process.waitForStarted(5000)) {
        return failStart();
    }
    if (!m_fd.accept(&e, m_process.processId())) {
        return failStart();
    }
    for (int i = 0; i < 60 && !m_ctrl.valid(); ++i) {
        if (m_ctrl.connect(m_socketPath.toStdString(), m_process.processId(), &e))
            break;
        QThread::msleep(50);
    }
    if (!m_ctrl.valid()) {
        return failStart();
    }
    auto pong = call(PingRequest { std::string(IPC::COMPAT) });
    if (!pong || pong->error) {
        return failStart();
    }
    auto* p = std::get_if<PingResponse>(&pong->payload);
    if (!p || p->compat != IPC::COMPAT) {
        return failStart();
    }
    if (sandboxStatus)
        *sandboxStatus = p->sandbox;
    // The notifier is enabled only while no synchronous RPC is in flight;
    // call() drains notifications as part of its response loop.
    m_notifier = std::make_unique<QSocketNotifier>(m_ctrl.fd(), QSocketNotifier::Type::Read, this);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &WorkerTransport::processIncomingNotifications);
    return true;
}

void WorkerTransport::cleanupSession()
{
    // Cleanup is idempotent because it is used for normal stop, failed start,
    // abort, and unexpected worker exit.
    ++m_frameSession;
    m_frameSlots.clear();
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier.reset();
    }
    m_ctrl.close();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(3000)) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }
    m_fd.close();
    // Plugin generated these paths, so it owns cleanup regardless of how the worker exited.
    if (!m_socketPath.isEmpty()) {
        QFile::remove(m_socketPath);
        m_socketPath.clear();
    }
    if (!m_fdSocketPath.isEmpty()) {
        QFile::remove(m_fdSocketPath);
        m_fdSocketPath.clear();
    }
    if (!m_tempPath.isEmpty()) {
        QFile::remove(m_tempPath);
        m_tempPath.clear();
    }
    m_sourcePath.clear();
    m_useEpubCache = false;
    m_activeSignPassword.fill(u'\0');
    m_activeSignPassword.clear();
    m_inFlight = false;
#ifdef MU_DEBUG_ENABLED
    m_pageLinksStartedAt.reset();
#endif
}

void WorkerTransport::stop()
{
    m_intentionalStop = true;
    cleanupSession();
}

void WorkerTransport::abort()
{
    m_intentionalStop = false;
    cleanupSession();
}

bool WorkerTransport::isConnected() const
{
    return m_ctrl.valid() && m_process.state() == QProcess::Running;
}

OpenStatus
WorkerTransport::open(const QString& path, const QString& password, QList<PageInfo>* pages, DocumentType type)
{
    return openFile(path, password, pages, type, true);
}

OpenStatus WorkerTransport::openFile(const QString& path,
                                     const QString& password,
                                     QList<PageInfo>* pages,
                                     DocumentType type,
                                     bool useEpubAcceleratorCache)
{
#ifdef MU_DEBUG_ENABLED
    const auto pageLinksStartedAt = std::chrono::steady_clock::now();
#endif
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return OpenStatus::Failed;
    const auto transfer = m_nextTransfer++;
    std::string e;
    if (!m_fd.send(transfer, file.handle(), &e))
        return OpenStatus::Failed;
    std::optional<Caching::EPUB::CacheEntry> cached;
    if (useEpubAcceleratorCache && type == DocumentType::Epub) {
        cached = Caching::EPUB::Cache::load(path, m_settings);
    }
    std::vector<std::uint8_t> accelerator;
    if (cached && cached->accelerator)
        accelerator.assign(cached->accelerator->cbegin(), cached->accelerator->cend());
    OpenRequest req {
        { transfer }, QFileInfo(path).fileName().toStdString(), password.toStdString(), type, accelerator
    };
    m_linkGeneration = 0;
    auto response = call(req);
    if (!response || response->error) {
        if (response && response->error)
            MU_LOG(warning, "Mu::Plugin", std::string("open failed: ") + response->error->message);
        else
            MU_LOG(warning, "Mu::Plugin", "open produced no response");
        return response && response->error && response->error->code == ErrorCode::PermissionDenied
            ? OpenStatus::NeedsPassword
            : OpenStatus::Failed;
    }
    auto* opened = std::get_if<OpenResponse>(&response->payload);
    if (!opened)
        return OpenStatus::Failed;
    m_frameSlots.clear();
    if (useEpubAcceleratorCache && type == DocumentType::Epub && !opened->epubAccelerator.empty()) {
        const QByteArray produced(reinterpret_cast<const char*>(opened->epubAccelerator.data()),
                                  static_cast<qsizetype>(opened->epubAccelerator.size()));
        (void)Caching::EPUB::Cache::saveAccelerator(path, m_settings, produced);
    }
    m_linkGeneration = opened->linkGeneration;
#ifdef MU_DEBUG_ENABLED
    m_pageLinksStartedAt = pageLinksStartedAt;
#endif
    if (pages) {
        pages->clear();
        pages->reserve(static_cast<qsizetype>(opened->pages.size()));
        for (auto& page : opened->pages)
            pages->append(std::move(page));
    }
    m_sourcePath = path;
    m_useEpubCache = useEpubAcceleratorCache && type == DocumentType::Epub;
    return OpenStatus::Success;
}

OpenStatus
WorkerTransport::openData(const QByteArray& data, const QString& password, QList<PageInfo>* pages, DocumentType type)
{
    // The worker receives an FD, not the QByteArray. Stage the data in a
    // plugin-owned file and let cleanupSession remove it on every exit path.
    if (!m_tempPath.isEmpty()) {
        QFile::remove(m_tempPath);
        m_tempPath.clear();
    }
    QTemporaryFile file(Util::tempDirectory() + QStringLiteral("/data-XXXXXX.pdf"));
    if (!file.open() || file.write(data) != data.size() || !file.flush())
        return OpenStatus::Failed;
    file.setAutoRemove(false);
    m_tempPath = file.fileName();
    const OpenStatus status = openFile(m_tempPath, password, pages, type, false);
    if (status == OpenStatus::Failed) {
        QFile::remove(m_tempPath);
        m_tempPath.clear();
    }
    return status;
}

bool WorkerTransport::close()
{
    // Document state belongs to the worker, while staged input belongs to the
    // plugin; clear both even when the close RPC fails.
    auto response = call(CloseRequest { });
    if (response && !response->error)
        m_frameSlots.clear();
    if (!m_tempPath.isEmpty()) {
        QFile::remove(m_tempPath);
        m_tempPath.clear();
    }
    m_sourcePath.clear();
    m_useEpubCache = false;
    m_linkGeneration = 0;
#ifdef MU_DEBUG_ENABLED
    m_pageLinksStartedAt.reset();
#endif
    return response && !response->error;
}

QImage WorkerTransport::render(int page, int width, int height, const QRect& rect)
{
    RenderRequest request { page, width, height, std::nullopt };
    if (!rect.isEmpty())
        request.tile = RenderTile { rect.x(), rect.y(), rect.width(), rect.height() };
    const auto id = m_nextId;
    auto response = call(request);
    if (!response || response->error)
        return { };
    auto* render = std::get_if<RenderResponse>(&response->payload);
    if (!render)
        return { };

    const auto validFrame = [&](const void* mapping, std::size_t size) {
        return IPC::validateRenderFrame(static_cast<const IPC::FrameBufferHeader*>(mapping), render->frame, id, size);
    };
    const auto createImage = [&](const void* mapping, QImageCleanupFunction cleanup, void* cleanupData) {
        const auto* pixels = static_cast<const uchar*>(IPC::framePixelData(mapping));
        QImage image(pixels,
                     render->frame.width,
                     render->frame.height,
                     render->frame.stride,
                     QImage::Format_RGBA8888,
                     cleanup,
                     cleanupData);
        if (image.isNull())
            cleanup(cleanupData);
        return image;
    };

    if (render->frame.slotId) {
        if (!render->frame.leaseId)
            return { };
        const auto rejectSlot = [&] {
            releaseFrameSlot(m_frameSession, render->frame.slotId, render->frame.leaseId);
            return QImage { };
        };

        std::shared_ptr<FrameSlotMapping> slot;
        const auto known = m_frameSlots.find(render->frame.slotId);
        if (known != m_frameSlots.end()) {
            if (render->frame.transferId)
                return rejectSlot();
            slot = known->second;
        } else {
            if (!render->frame.transferId)
                return rejectSlot();
            std::string error;
            const int fd = m_fd.receive(render->frame.transferId, &error);
            if (fd < 0) {
                MU_LOG(warning, "Mu::Plugin", std::string("render slot receive failed: ") + error);
                return rejectSlot();
            }
            struct stat info { };
            if (fstat(fd, &info) || info.st_size < static_cast<off_t>(sizeof(IPC::FrameBufferHeader))) {
                ::close(fd);
                return rejectSlot();
            }
            const auto size = static_cast<std::size_t>(info.st_size);
            void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
            ::close(fd);
            if (mapping == MAP_FAILED)
                return rejectSlot();
            slot = std::make_shared<FrameSlotMapping>();
            slot->mapping = mapping;
            slot->size = size;
        }

        if (!validFrame(slot->mapping, slot->size))
            return rejectSlot();
        if (known == m_frameSlots.end() && !m_frameSlots.emplace(render->frame.slotId, slot).second)
            return rejectSlot();
        auto* lease = new FrameLease { this, m_frameSession, render->frame.slotId, render->frame.leaseId, slot };
        return createImage(slot->mapping, cleanupFrameLease, lease);
    }

    if (!render->frame.transferId)
        return { };
    std::string error;
    const int fd = m_fd.receive(render->frame.transferId, &error);
    if (fd < 0) {
        MU_LOG(warning, "Mu::Plugin", std::string("render receive failed: ") + error);
        return { };
    }
    const auto rejectFrame = [&](void* mapping = MAP_FAILED, std::size_t size = 0) {
        if (mapping != MAP_FAILED)
            ::munmap(mapping, size);
        ::close(fd);
        return QImage { };
    };
    struct stat info { };
    if (fstat(fd, &info) || info.st_size < static_cast<off_t>(sizeof(IPC::FrameBufferHeader)))
        return rejectFrame();
    const auto size = static_cast<std::size_t>(info.st_size);
    void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
        return rejectFrame();
    if (!validFrame(mapping, size))
        return rejectFrame(mapping, size);

    ::close(fd);
    auto* mappedFrame = new MappedFrame { mapping, size };
    return createImage(mapping, cleanupMappedFrame, mappedFrame);
}

std::vector<TextBox> WorkerTransport::getTextBoxesForPage(int page, qreal x, qreal y, bool skipAnnots)
{
    auto response = call(TextBoxesRequest { page, x, y, skipAnnots });
    if (!response || response->error)
        return { };
    if (auto* boxes = std::get_if<TextBoxesResponse>(&response->payload))
        return std::move(boxes->boxes);
    return { };
}

bool WorkerTransport::sendOcrInput(QFile& input, std::uint64_t& transfer)
{
    // OCR uses the currently opened document as an FD transfer; no document
    // path or contents are copied into the control protocol.
    if (m_sourcePath.isEmpty() || !input.open(QIODevice::ReadOnly))
        return false;
    transfer = m_nextTransfer++;
    std::string error;
    return m_fd.send(transfer, input.handle(), &error);
}

std::optional<ResponseMessage> WorkerTransport::requestOcr(int page, const QString& language, int dpi, bool async)
{
    QFile input(m_sourcePath);
    std::uint64_t transfer = 0;
    if (!sendOcrInput(input, transfer))
        return std::nullopt;
    return call(OcrPageRequest { { transfer }, page, dpi, language.toStdString(), async });
}

OcrResult WorkerTransport::ocrPage(int page, const QString& language, int dpi, bool async)
{
    auto response = requestOcr(page, language, dpi, async);
    if (!response || response->error)
        return { };
    if (auto* result = std::get_if<OcrResponse>(&response->payload))
        return result->result;
    if (std::holds_alternative<JobResponse>(response->payload)) {
        return { OcrStatus::Success, { } };
    }
    return { };
}

std::optional<quint64> WorkerTransport::startOcrPage(int page, const QString& language, int dpi)
{
    auto response = requestOcr(page, language, dpi, true);
    if (!response || response->error)
        return std::nullopt;
    if (auto* job = std::get_if<JobResponse>(&response->payload))
        return job->jobId;
    return std::nullopt;
}

OcrResult WorkerTransport::ocrResult(quint64 id)
{
    auto response = call(OcrResultRequest { id });
    if (!response || response->error)
        return { };
    if (auto* result = std::get_if<OcrResponse>(&response->payload))
        return result->result;
    return { };
}

bool WorkerTransport::cancelOcrJobs()
{
    const auto response = call(CancelOcrJobsRequest { });
    return response && !response->error;
}

std::vector<Font> WorkerTransport::fonts(int page)
{
    auto response = call(FontsRequest { page });
    if (!response || response->error)
        return { };
    if (auto* value = std::get_if<FontsResponse>(&response->payload))
        return std::move(value->fonts);
    return { };
}

std::vector<EmbeddedFile> WorkerTransport::embeddedFiles()
{
    auto response = call(EmbeddedFilesRequest { });
    if (!response || response->error)
        return { };
    if (auto* value = std::get_if<EmbeddedFilesResponse>(&response->payload))
        return std::move(value->files);
    return { };
}

std::vector<OutlineNode> WorkerTransport::synopsis()
{
    if (m_useEpubCache && !m_sourcePath.isEmpty()) {
        if (const auto cached = Caching::EPUB::Cache::load(m_sourcePath, m_settings); cached && cached->outline) {
            return *cached->outline;
        }
    }
    auto response = call(SynopsisRequest { });
    if (!response || response->error)
        return { };
    if (auto* value = std::get_if<OutlineResponse>(&response->payload)) {
        if (m_useEpubCache && !m_sourcePath.isEmpty())
            (void)Caching::EPUB::Cache::saveOutline(m_sourcePath, m_settings, value->nodes);
        return std::move(value->nodes);
    }
    return { };
}

DocumentMetadata WorkerTransport::getDocumentInfo(const QStringList& keys)
{
    std::vector<std::string> values;
    for (const auto& key : keys)
        values.push_back(key.toStdString());
    auto response = call(MetadataRequest { std::move(values) });
    if (!response || response->error)
        return { };
    if (auto* value = std::get_if<MetadataResponse>(&response->payload))
        return value->metadata;
    return { };
}

std::optional<AnnotationHandle> WorkerTransport::addAnnotation(int page, const Annotation& annotation)
{
    auto response = call(AnnotationAddRequest { page, annotation });
    if (!response || response->error)
        return std::nullopt;
    if (auto* value = std::get_if<AnnotationResponse>(&response->payload))
        return value->handle;
    return std::nullopt;
}

bool WorkerTransport::modifyAnnotation(int page, const QString& handle, const Annotation& annotation, bool appearance)
{
    auto response = call(AnnotationModifyRequest { { page, { handle.toStdString() }, annotation, appearance } });
    return response && !response->error;
}

bool WorkerTransport::removeAnnotation(int page, const QString& handle)
{
    auto response = call(AnnotationRemoveRequest { page, { handle.toStdString() } });
    return response && !response->error;
}

bool WorkerTransport::saveToFile(const QString& target)
{
    return writeFile(SaveRequest { }, target);
}

bool WorkerTransport::savePdfToFile(const QString& target, const QVector<int>& pages)
{
    std::vector<std::int32_t> pageList;
    pageList.reserve(static_cast<std::size_t>(pages.size()));
    for (int p : pages)
        pageList.push_back(p);
    return writeFile(SavePdfRequest { { }, std::move(pageList) }, target);
}

SignResponse WorkerTransport::signToFile(SignRequest request, const QString& password, const QString& target)
{
    QFileInfo info(target);
    QTemporaryFile file(info.absolutePath() + QStringLiteral("/.mupdf-worker-XXXXXX"));
    if (!file.open())
        return { SigningResult::WriteFailed, "could not create signing output" };
    const auto transfer = m_nextTransfer++;
    std::string error;
    if (!m_fd.send(transfer, file.handle(), &error))
        return { SigningResult::WriteFailed, error };
    request.file.transferId = transfer;
    m_activeSignPassword = password;
    auto response = call(std::move(request));
    m_activeSignPassword.fill(u'\0');
    m_activeSignPassword.clear();
    if (!response || response->error)
        return { SigningResult::GenericError,
                 response && response->error ? response->error->message : "worker failed to sign" };
    const auto* result = std::get_if<SignResponse>(&response->payload);
    if (!result)
        return { SigningResult::GenericError, "worker returned an invalid signing response" };
    if (result->result != SigningResult::Success)
        return *result;
    if (!file.flush() || fsync(file.handle()))
        return { SigningResult::WriteFailed, "could not flush signed output" };
    file.setAutoRemove(false);
    if (::rename(QFile::encodeName(file.fileName()).constData(), QFile::encodeName(target).constData())) {
        file.setAutoRemove(true);
        return { SigningResult::WriteFailed, "could not replace signing output" };
    }
    return *result;
}

std::optional<FormUpdateResponse> WorkerTransport::updateForm(const FormUpdateRequest& request)
{
    auto response = call(request);
    if (response && std::holds_alternative<FormUpdateResponse>(response->payload))
        return std::get<FormUpdateResponse>(std::move(response->payload));
    return std::nullopt;
}

std::optional<FormUpdateResponse> WorkerTransport::resetForm(const FormResetRequest& request)
{
    auto response = call(request);
    if (response && std::holds_alternative<FormUpdateResponse>(response->payload))
        return std::get<FormUpdateResponse>(std::move(response->payload));
    return std::nullopt;
}

bool WorkerTransport::settings(const DocumentSettings& settings)
{
    auto response = call(SettingsRequest { settings });
    if (!response || response->error)
        return false;
    m_settings = settings;
    return true;
}

std::optional<ResponseMessage> WorkerTransport::call(RequestPayload payload)
{
    // The protocol permits one synchronous request at a time. During that
    // request, asynchronous notifications are decoded and handled inline so
    // the response stream remains ordered.
    if (!isConnected())
        return { };

    if (m_inFlight) {
        MU_LOG(critical, "Mu::Plugin", "concurrent or reentrant RPC call detected; aborting transport");
        abort();
        return { };
    }

    const auto abortWith = [&](const std::string& reason, bool critical = false) -> std::optional<ResponseMessage> {
        if (critical)
            MU_LOG(critical, "Mu::Plugin", reason + "; aborting transport");
        else
            MU_LOG(warning, "Mu::Plugin", reason + "; aborting transport");
        abort();
        return { };
    };

    if (m_notifier)
        m_notifier->setEnabled(false);

    m_inFlight = true;

    struct InFlightGuard {
        WorkerTransport* transport;

        ~InFlightGuard()
        {
            // Re-enable idle notifications only after the response exchange
            // has fully restored the transport's serialized state.
            transport->m_inFlight = false;
            if (transport->m_notifier && transport->isConnected()) {
                transport->m_notifier->setEnabled(true);
                transport->processIncomingNotifications();
            }
        }
    } guard { this };

    const auto id = m_nextId++;
    RequestMessage request { id, std::move(payload) };
    MU_LOG(debug, "Plugin -> Worker", IPC::Debug::request(request, true));
    std::string e;
    if (!IPC::ZppCodec::writeMessage(m_ctrl, request, Timeout::ControlWriteMs, &e, "plugin"))
        return abortWith("request write failed: " + e);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(timeoutFor(request.payload));
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            return abortWith("request timed out id=" + std::to_string(id));

        std::vector<std::byte> frame;
        if (!IPC::readFrame(m_ctrl, &frame, static_cast<int>(remaining), &e))
            return abortWith("response read failed: " + e);

        MU_LOG(debug, "Mu::Plugin", "read frame bytes=" + std::to_string(frame.size()));
        ResponseMessage response;
        if (IPC::ZppCodec::decode(frame, &response, &e)) {
            if (response.id == id) {
#ifdef MU_DEBUG_ENABLED
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                        .count();
                MU_LOG(debug,
                       "Plugin <- Worker",
                       IPC::Debug::response(response, true) + " elapsedMs=" + std::to_string(elapsed));
#endif
                return response;
            }
            return abortWith("protocol stream desynchronization: expected response id=" + std::to_string(id)
                                 + " but received id=" + std::to_string(response.id),
                             true);
        }

        NotificationMessage notification;
        if (!IPC::ZppCodec::decode(frame, &notification, &e))
            return abortWith("protocol message decode failed: " + e, true);

        if (!handleNotification(notification, &e))
            return abortWith(e);
    }
}

bool WorkerTransport::handleNotification(const NotificationMessage& notification, std::string* error)
{
    // Notifications are worker-to-plugin events. Signing input is the only
    // notification that requires a reply on the same control channel.
#ifdef MU_DEBUG_ENABLED
    std::int64_t elapsedMs = -1;
    if (const auto* links = std::get_if<PageLinksNotification>(&notification.payload);
        links && links->generation == m_linkGeneration && m_pageLinksStartedAt) {
        elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                                                          - *m_pageLinksStartedAt)
                        .count();
        m_pageLinksStartedAt.reset();
    }
    auto detail = IPC::Debug::notification(notification, true);
    if (elapsedMs >= 0)
        detail += " elapsedMs=" + std::to_string(elapsedMs);
    MU_LOG(debug, "Plugin <- Worker", detail);
#endif

    if (auto* done = std::get_if<OcrDoneNotification>(&notification.payload)) {
        Q_EMIT ocrDone(done->jobId, done->page);
        return true;
    }
    if (auto* links = std::get_if<PageLinksNotification>(&notification.payload)) {
        if (links->generation == m_linkGeneration)
            Q_EMIT pageLinksReady(
                links->generation, links->pages, links->resourceLimited, QString::fromStdString(links->error));
        return true;
    }
    if (auto* sign = std::get_if<SignInput>(&notification.payload)) {
        const auto cms = Crypto::createDetachedCmsFromDigest(
            QString::fromStdString(sign->certificateNickname), m_activeSignPassword, sign->digest);
        SignReply reply { sign->jobId,
                          sign->nonce,
                          cms.result,
                          cms.details.toStdString(),
                          std::vector<std::uint8_t>(cms.cms.cbegin(), cms.cms.cend()) };
        RequestMessage replyMessage { m_nextId++, reply };
        MU_LOG(debug, "Plugin -> Worker", IPC::Debug::request(replyMessage, true));
        std::string e;
        if (!IPC::ZppCodec::writeMessage(m_ctrl, replyMessage, Timeout::SignRoundTripMs, &e, "plugin")) {
            if (error)
                *error = "sign reply write failed: " + e;
            return false;
        }
        return true;
    }
    return true;
}

void WorkerTransport::processIncomingNotifications()
{
    // Idle notifications are drained until the nonblocking channel has no
    // complete frame left; malformed or unexpected messages abort the session.
    if (m_inFlight || !m_ctrl.valid())
        return;

    std::string e;
    for (;;) {
        std::vector<std::byte> frame;
        const auto status = IPC::tryReadFrame(m_ctrl, &frame, &e);
        if (status == IPC::ReadStatus::NoData || status == IPC::ReadStatus::Partial)
            break;
        if (status == IPC::ReadStatus::Closed) {
            MU_LOG(warning, "Mu::Plugin", "worker control socket closed while idle");
            abort();
            break;
        }
        if (status == IPC::ReadStatus::Error) {
            MU_LOG(warning, "Mu::Plugin", "worker control socket read error while idle: " + e);
            abort();
            break;
        }
        if (status == IPC::ReadStatus::Complete) {
            NotificationMessage notification;
            if (IPC::ZppCodec::decode(frame, &notification, &e)) {
                if (!handleNotification(notification, &e)) {
                    MU_LOG(warning, "Mu::Plugin", "handling idle notification failed: " + e + "; aborting transport");
                    abort();
                    break;
                }
                continue;
            }
            MU_LOG(critical,
                   "Mu::Plugin",
                   "unexpected non-notification message while transport is idle; aborting transport");
            abort();
            break;
        }
    }
}

QString WorkerTransport::findBinary(const QString& hint)
{
    if (!hint.isEmpty() && QFileInfo::exists(hint))
        return hint;
#ifdef WORKER_BUILD_PATH
    const QString build = QStringLiteral(WORKER_BUILD_PATH);
    if (QFileInfo::exists(build))
        return build;
#endif
#ifdef WORKER_INSTALL_PATH
    const QString install = QStringLiteral(WORKER_INSTALL_PATH);
    if (QFileInfo::exists(install))
        return install;
#endif
    return QStandardPaths::findExecutable(QStringLiteral("okular-mupdf-worker"));
}

void WorkerTransport::finished(int code, QProcess::ExitStatus /*status*/)
{
    // Intentional stop/abort already performed cleanup; only an unplanned
    // process exit is surfaced to the client for recovery.
    if (!m_intentionalStop)
        Q_EMIT workerDied(code);
}

} // namespace Mu::Plugin
