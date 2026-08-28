// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <argparse/argparse.hpp>
#include <mupdf/fitz/version.h>

#include "runtime/worker_server.hpp"
#include "shared/logging.hpp"
#include "shared/transport/compat.hpp"
#include "shared/transport/fd_channel.hpp"
#include "sys/sandbox.hpp"

namespace {

constexpr std::string_view DefaultTessDataDirectory = TESSDATA_DIR;

std::string makeAbsolutePath(std::string_view path)
{
    if (path.empty())
        return { };
    std::error_code error;
    const auto absolutePath = std::filesystem::absolute(std::filesystem::path(path), error);
    return error ? std::string(path) : absolutePath.lexically_normal().string();
}

void appendUniquePath(std::vector<std::string>& paths, std::string path)
{
    if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end())
        paths.push_back(std::move(path));
}

// Argument parser using argparse.hpp
void configureArgumentParser(argparse::ArgumentParser& arguments)
{
    arguments.add_description(
        "Sandboxed MuPDF worker for the okular-mupdf-ng Okular generator. It processes PDF and EPUB documents over "
        "private IPC.");
    arguments.add_epilog(
        "This program is launched by the Okular plugin. It does not open document paths or render documents directly.");
    arguments.add_argument("--socket")
        .metavar("PATH")
        .help("control Unix-domain socket for private plugin IPC")
        .required();
    arguments.add_argument("--fd-socket")
        .metavar("PATH")
        .help("Unix-domain socket for private descriptor transfer")
        .required();
    arguments.add_argument("--tessdata-dir").metavar("PATH").help("Tesseract data directory").append();
}

} // namespace

/// Entry point for the isolated MuPDF rendering worker subprocess.
///
/// Lifecycle Overview:
/// 1. Parse IPC endpoint arguments (`--socket` for framed serialized control messages, `--fd-socket` for SCM_RIGHTS FD
/// transfers).
/// 2. Establish connection to the parent generator process via the auxiliary FD channel socket.
/// 3. Bind and listen on the primary control domain socket to accept plugin commands.
/// 4. Lockdown the process using Linux Landlock, Seccomp-BPF filters, resource limits, and namespace drops.
/// 5. Enter the non-blocking polling event loop in `WorkerServer::run` until termination or parent disconnect.
int main(int argc, char* argv[])
{
    // Step 1: Parse required command-line IPC socket paths passed by the plugin host.
    argparse::ArgumentParser arguments("okular-mupdf-worker",
                                       std::string("okular-mupdf-worker ") + std::string(::Mu::IPC::COMPAT) + "\nMuPDF "
                                           + FZ_VERSION);
    configureArgumentParser(arguments);
    std::vector<std::string> commandLine;
    commandLine.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
        commandLine.emplace_back(argv[index]);

    try {
        arguments.parse_args(commandLine);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n\n" << arguments;
        return 2;
    }
    const auto socketPath = arguments.get<std::string>("--socket");
    const auto fdSocketPath = arguments.get<std::string>("--fd-socket");
    // Keep the build-time default first: sandbox activation requires it, while
    // command-line directories are optional additional read-only tessdata trees.
    std::vector<std::string> tessDataDirectories { makeAbsolutePath(DefaultTessDataDirectory) };
    if (const auto configuredDirectories = arguments.present<std::vector<std::string>>("--tessdata-dir")) {
        for (const auto& directory : *configuredDirectories)
            appendUniquePath(tessDataDirectories, makeAbsolutePath(directory));
    }

    // Step 2: Connect auxiliary file descriptor channel to the parent generator process.
    // This channel uses SCM_RIGHTS to receive document file descriptors and shared memory frames.
    ::Mu::IPC::FdChannel fdChannel;
    std::string error;
    if (!fdChannel.connect(fdSocketPath.c_str(), &error, ::getppid())) {
        MU_LOG(critical, "Mu::Worker", std::string("failed to connect FD channel: ") + error);
        return 2;
    }

    // Step 3: Create and start control socket listener to service RPC commands.
    ::Mu::Worker::Runtime::WorkerServer server(socketPath.c_str(), &fdChannel, { }, ::getppid());
    if (!server.listen(&error)) {
        MU_LOG(critical, "Mu::Worker", std::string("failed to start control socket: ") + error);
        return 2;
    }

    // Step 4: Activate namespaces, resource limits, Landlock, and Seccomp.
    // Landlock leaves read-only access to the configured tessdata directories for
    // Tesseract. Document and output access instead use descriptors transferred
    // over the FD channel; Seccomp prevents creating new filesystem or network endpoints.
    const std::vector<int> preservedFds = { fdChannel.fd(), server.controlSocketFd() };
    const auto sandbox = ::Mu::Worker::Sandbox::activate(tessDataDirectories, preservedFds);
    server.setSandboxStatus(sandbox);

    // Best-effort sandboxing is intentional: worker availability is preferred
    // on hosts where some Linux hardening controls are unavailable. The parent
    // can inspect the degraded status reported through Ping.
    if (!sandbox.isPartiallyActive()) {
        MU_LOG(critical, "Mu::Worker", std::string("worker unconfined; all sandboxing failed: ") + sandbox.reason);
    } else if (!sandbox.isFullyHardened()) {
        MU_LOG(warning, "Mu::Worker", std::string("sandbox partially active: ") + sandbox.reason);
    }

    // Step 5: Enter main multiplexing event loop, handling requests until client disconnects or exits.
    return server.run(&error);
}
