// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_RUNTIME_WORKER_SERVER_HPP
#define MU_WORKER_RUNTIME_WORKER_SERVER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "runtime/command_service.hpp"
#include "shared/transport/common.hpp"
#include "shared/transport/ctrl_channel.hpp"
#include "shared/transport/fd_channel.hpp"
#include "sys/sandbox.hpp"

namespace Mu::Worker::Runtime {

using ::Mu::IPC::CtrlChannel;
using ::Mu::IPC::FdChannel;

/**
 * Native control-plane server managing the isolated worker process lifecycle.
 *
 * Responsibilities:
 * 1. Binds to the local UNIX control socket and authenticates incoming plugin connections by peer PID.
 * 2. Runs the main non-blocking polling event loop, multiplexing incoming control frames and OCR completion eventfds.
 * 3. Dispatches client RPC requests to CommandService and serializes responses over ZppCodec.
 * 4. Pushes asynchronous background events (such as OCR completion notifications) back to the plugin host.
 */
class WorkerServer {
public:
    WorkerServer(std::string socketPath, FdChannel* fdChannel, Sandbox::Status sandbox, std::int64_t expectedPluginPid);
    ~WorkerServer();

    WorkerServer(const WorkerServer&) = delete;
    WorkerServer& operator=(const WorkerServer&) = delete;

    /// Binds and listens on the configured UNIX domain socket path.
    bool listen(std::string* error = nullptr);

    /// Updates the active sandbox capability status reported to client pings.
    void setSandboxStatus(Sandbox::Status sandbox);

    /// Returns the listening control socket raw file descriptor.
    [[nodiscard]] int controlSocketFd() const noexcept { return m_server.fd(); }

    /// Runs the main event loop, multiplexing control socket commands and OCR notifications.
    /// Returns 0 on clean disconnect, non-zero on fatal errors.
    int run(std::string* error = nullptr);

private:
    /// Accepts and authenticates an incoming plugin connection against the expected parent PID.
    bool acceptClient(std::string* error, ::Mu::IPC::IoResult* result = nullptr);

    /// Reads, decodes, and dispatches a single incoming control frame.
    /// When `deferred` is present, it is a frame already read during a nested
    /// signing exchange and is dispatched without another socket read.
    bool processFrame(std::string* error, std::optional<std::vector<std::byte>> deferred = std::nullopt);

    /// Serializes and writes a ResponseMessage to the active client control socket.
    /// Replaces an oversized response with a frame-sized ResourceLimit response.
    bool writeResponse(const ::Mu::Model::ResponseMessage& response, std::string* error);

    /// Drains and transmits pending asynchronous OCR completion notifications to the client.
    bool writeOcrNotifications(std::string* error);

    /// Sends the final incremental page-link aggregate or its terminal error notification.
    bool writePageLinks(std::string* error);

    /// Sole owner of active-client teardown; resets command state before closing the channel.
    void disconnectClient();

    std::string m_socketPath;
    std::int64_t m_expectedPluginPid = -1;
    FdChannel* m_fdChannel = nullptr;
    Sandbox::Status m_sandbox;
    CtrlChannel m_server;
    std::unique_ptr<CtrlChannel> m_client;
    std::unique_ptr<CommandService> m_commandService;
};

} // namespace Mu::Worker::Runtime

#endif // MU_WORKER_RUNTIME_WORKER_SERVER_HPP
