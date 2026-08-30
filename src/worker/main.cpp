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

#include <cxxopts/cxxopts.hpp>
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

cxxopts::Options makeWorkerOptions()
{
    cxxopts::Options options(
        "okular-mupdf-worker",
        "Sandboxed MuPDF worker for the okular-mupdf-ng Okular generator.");
    options.add_options()("socket", "control Unix-domain socket for private plugin IPC", cxxopts::value<std::string>(), "PATH")(
        "fd-socket", "Unix-domain socket for private descriptor transfer", cxxopts::value<std::string>(), "PATH")(
        "tessdata-dir", "Tesseract data directory", cxxopts::value<std::vector<std::string>>(), "PATH")(
        "h,help", "Print usage")("version", "Print version");
    options.custom_help("[OPTION...]");
    return options;
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
    auto options = makeWorkerOptions();
    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << "\n";
            std::cout << "This program is launched by the Okular plugin. It does not open document paths or render documents directly.\n";
            return 0;
        }
        if (result.count("version")) {
            std::cout << "okular-mupdf-worker " << ::Mu::IPC::COMPAT << "\nMuPDF " << FZ_VERSION << "\n";
            return 0;
        }
        if (!result.count("socket")) {
            throw cxxopts::exceptions::missing_argument("socket");
        }
        if (!result.count("fd-socket")) {
            throw cxxopts::exceptions::missing_argument("fd-socket");
        }
    } catch (const cxxopts::exceptions::exception& error) {
        std::cerr << "error: " << error.what() << "\n\n" << options.help() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n\n" << options.help() << "\n";
        return 2;
    }
    const auto socketPath = result["socket"].as<std::string>();
    const auto fdSocketPath = result["fd-socket"].as<std::string>();
    // Keep the build-time default first: sandbox activation requires it, while
    // command-line directories are optional additional read-only tessdata trees.
    std::vector<std::string> tessDataDirectories { makeAbsolutePath(DefaultTessDataDirectory) };
    if (result.count("tessdata-dir")) {
        for (const auto& directory : result["tessdata-dir"].as<std::vector<std::string>>())
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
