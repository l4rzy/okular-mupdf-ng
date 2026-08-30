// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/worker_server.hpp"

#include <cerrno>
#ifdef MU_DEBUG_ENABLED
#include <chrono>
#endif
#include <cstring>
#include <poll.h>
#include <unistd.h>

#include "shared/logging.hpp"
#include "shared/protocol/ipc_debug.hpp"
#include "shared/protocol/zpp_codec.hpp"
#include "shared/transport/poll.hpp"
#include "sys/sys.hpp"

using namespace Mu;

namespace Mu::Worker::Runtime {

namespace ZppCodec = ::Mu::IPC::ZppCodec;
using ::Mu::IPC::CtrlChannel;
using ::Mu::IPC::FdChannel;
using ::Mu::IPC::MonotonicDeadline;
using ::Mu::IPC::PollLoop;
namespace Timeout = ::Mu::IPC::Timeout;
using ::Mu::Model::NotificationMessage;
using ::Mu::Model::OcrDoneNotification;
using ::Mu::Model::RequestMessage;
using ::Mu::Model::ResponseMessage;
using Sandbox::Status;

namespace {

constexpr int AcceptTimeoutMs = 1000;

}

// =============================================================================
// Construction & Socket Listening
// =============================================================================

WorkerServer::WorkerServer(std::string socketPath,
                           FdChannel* fdChannel,
                           Sandbox::Status sandbox,
                           std::int64_t expectedPluginPid)
    : m_socketPath(std::move(socketPath))
    , m_expectedPluginPid(expectedPluginPid)
    , m_fdChannel(fdChannel)
    , m_sandbox(std::move(sandbox))
{
}

WorkerServer::~WorkerServer() = default;

bool WorkerServer::listen(std::string* error)
{
    return m_server.listen(m_socketPath, error);
}

void WorkerServer::setSandboxStatus(Sandbox::Status sandbox)
{
    m_sandbox = std::move(sandbox);
}

// =============================================================================
// Client Connection & Session Management
// =============================================================================

// Each connection gets a fresh CommandService, so a newly connected plugin
// always starts with an empty document and annotation state; the sandbox
// status and FD channel are wired in at connect time.
bool WorkerServer::acceptClient(std::string* error, IPC::IoResult* result)
{
    auto client = m_server.accept(m_expectedPluginPid, AcceptTimeoutMs, error, result);
    if (!client.valid())
        return false;

    m_client = std::make_unique<CtrlChannel>(std::move(client));
    m_commandService = std::make_unique<CommandService>(SessionContext {
        .sandbox = m_sandbox,
        .fdChannel = m_fdChannel,
        .controlChannel = m_client.get(),
    });
    return true;
}

void WorkerServer::disconnectClient()
{
    m_commandService.reset();
    m_client.reset();
}

// =============================================================================
// Frame Processing & Response Writing
// =============================================================================

bool WorkerServer::writeResponse(const ResponseMessage& response, std::string* error)
{
    std::string encodeError;
    ZppCodec::EncodeError encodeErrorCode = ZppCodec::EncodeError::None;
    const auto data = ZppCodec::encode(response, &encodeError, &encodeErrorCode);
    if (!data) {
        if (encodeErrorCode != ZppCodec::EncodeError::ControlMessageLimit) {
            if (error)
                *error = std::move(encodeError);
            return false;
        }

        // The original response cannot fit in a control message. Preserve its
        // request ID so the plugin receives a normal terminal response instead.
        const ResponseMessage limitResponse { response.id,
                                              std::monostate { },
                                              ::Mu::Model::Error { ::Mu::Model::ErrorCode::ResourceLimit,
                                                                   "response",
                                                                   "response exceeds control-message limit" } };
        if (error)
            error->clear();
        return ZppCodec::writeMessage(*m_client, limitResponse, Timeout::ControlWriteMs, error, "worker");
    }

    MU_LOG(debug, "worker", std::string("write frame bytes=") + std::to_string(data->size()));
    return ::Mu::IPC::writeFrame(*m_client, *data, Timeout::ControlWriteMs, error);
}

bool WorkerServer::processFrame(std::string* error, std::optional<std::vector<std::byte>> deferred)
{
    // Step 1: Decode incoming request frame.
    // Frames buffered by a nested operation (e.g. the sign round trip) were
    // already received in order, so dispatch them before reading anything new.
    RequestMessage request;
    if (deferred) {
        if (!ZppCodec::decode(*deferred, &request, error))
            return false;
    } else if (!ZppCodec::readMessage(*m_client, &request, Timeout::ControlReadMs, error, "worker")) {
        return false;
    }

#ifdef MU_DEBUG_ENABLED
    MU_LOG(debug, "Worker <- Plugin", IPC::Debug::request(request, true));
    const auto started = std::chrono::steady_clock::now();
#endif

    // Step 2: Route request to CommandService for document engine execution
    const ResponseMessage response = m_commandService->dispatch(request);

    if (m_commandService->disconnectRequested()) {
        if (error)
            *error = "command service requested session disconnect";
        return false;
    }

#ifdef MU_DEBUG_ENABLED
    MU_LOG(debug,
           "Worker -> Plugin",
           IPC::Debug::response(response, true) + " elapsedMs="
               + std::to_string(
                   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                       .count()));
#endif

    // Step 3: Serialize and transmit the response back to the plugin host
    if (!writeResponse(response, error)) {
        MU_LOG(warning, "Mu::Worker", std::string("response write failed: ") + (error ? *error : "unknown error"));
        return false;
    }

    return true;
}

// =============================================================================
// Asynchronous Notification Flushing
// =============================================================================

bool WorkerServer::writeOcrNotifications(std::string* error)
{
    // Drain all queued completion events from the background OCR thread pool
    for (const auto& notification : m_commandService->drainOcrNotifications()) {
        const NotificationMessage message { OcrDoneNotification { notification.id, notification.page } };

        MU_LOG(debug, "Worker -> Plugin", IPC::Debug::notification(message, true));
        if (!ZppCodec::writeMessage(*m_client, message, Timeout::ControlWriteMs, error, "worker")) {
            MU_LOG(
                warning, "Mu::Worker", std::string("notification write failed: ") + (error ? *error : "unknown error"));
            return false;
        }
    }

    return true;
}

bool WorkerServer::writePageLinks(std::string* error)
{
    // CommandService advances one page at a time. It returns a notification only
    // after the current generation completes or encounters a terminal error.
    const auto notification = m_commandService->processPageLinks();
    if (!notification)
        return true;

    const NotificationMessage message { *notification };
    MU_LOG(debug, "Worker -> Plugin", IPC::Debug::notification(message, true));
    ZppCodec::EncodeError encodeError = ZppCodec::EncodeError::None;
    std::string serializationError;
    const auto data = ZppCodec::encode(message, &serializationError, &encodeError);
    if (!data && encodeError == ZppCodec::EncodeError::ControlMessageLimit) {
        m_commandService->cancelPageLinks();
        const NotificationMessage limitMessage { ::Mu::Model::PageLinksNotification {
            notification->generation,
            { },
            true,
            "page-link notification exceeds control-message limit",
        } };
        return ZppCodec::writeMessage(*m_client, limitMessage, Timeout::ControlWriteMs, error, "worker");
    }
    if (!data) {
        if (error)
            *error = std::move(serializationError);
        return false;
    }
    if (!::Mu::IPC::writeFrame(*m_client, *data, Timeout::ControlWriteMs, error)) {
        MU_LOG(warning,
               "Mu::Worker",
               std::string("page-link notification write failed: ") + (error ? *error : "unknown error"));
        return false;
    }
    return true;
}

// =============================================================================
// Main Event Loop
// =============================================================================

// Fuses the control socket with the OCR completion eventfd into one PollLoop, so
// notifications can be flushed while idle; accept timeouts are a retry signal,
// not an error.
int WorkerServer::run(std::string* error)
{
    for (;;) {
        // Step 1: Ensure active client connection
        if (!m_client) {
            std::string acceptError;
            IPC::IoResult ioResult = IPC::IoResult::Complete;
            if (!acceptClient(&acceptError, &ioResult)) {
                if (ioResult == IPC::IoResult::Timeout)
                    continue;
                if (error)
                    *error = std::move(acceptError);
                return 1;
            }
            MU_LOG(debug, "Mu::Worker", "authenticated plugin connected");
        }

        // Step 2: Dispatch any deferred requests captured during synchronous nested sub-calls
        // Never block in poll while frames buffered during a nested operation
        // still need dispatching; process them in receive order first.
        if (auto deferred = m_commandService->takeDeferredIncoming()) {
            std::string frameError;
            if (!processFrame(&frameError, std::move(deferred))) {
                MU_LOG(warning, "Mu::Worker", std::string("control frame failed: ") + frameError);
                disconnectClient();
                return 0;
            }
            continue;
        }

        bool disconnected = false;

        // Step 3: Configure PollLoop with control socket and OCR eventfd
        if (m_commandService->hasPendingPageLinks()) {
            std::string notificationError;
            if (!writePageLinks(&notificationError)) {
                disconnected = true;
            }
        }

        if (disconnected) {
            disconnectClient();
            return 0;
        }

        PollLoop loop;

        loop.watch(m_client->fd(), POLLIN | POLLERR | POLLHUP, [&](short revents) {
            if (revents & POLLIN) {
                std::string frameError;
                if (!processFrame(&frameError)) {
                    MU_LOG(warning, "Mu::Worker", std::string("control frame failed: ") + frameError);
                    disconnected = true;
                }
            } else if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                disconnected = true;
            }
        });

        if (m_commandService->ocrCompletionFd() >= 0) {
            loop.watch(m_commandService->ocrCompletionFd(), POLLIN, [&](short revents) {
                if (revents & POLLIN) {
                    std::string notificationError;
                    if (!writeOcrNotifications(&notificationError)) {
                        disconnected = true;
                    }
                }
            });
        }

        // Step 4: Run single poll cycle with infinite deadline
        auto deadline = m_commandService->hasPendingPageLinks() ? MonotonicDeadline::fromMilliseconds(0)
                                                                : MonotonicDeadline::never();
        const int ready = loop.runOnce(deadline, error);
        if (ready < 0) {
            return 1;
        }

        // Step 5: Handle session termination
        if (disconnected) {
            disconnectClient();
            return 0;
        }
    }
}

} // namespace Mu::Worker::Runtime
