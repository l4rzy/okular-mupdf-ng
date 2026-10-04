// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string_view>
#include <unistd.h>

#include "runtime/worker_server.hpp"

// Use the real command service and renderer, without sandbox activation: these
// tests stop/resume the process, which can require seccomp's restart_syscall.
// This executable is test-only and is never installed.
int main(int argc, char** argv)
{
    if (argc != 5 || std::string_view(argv[1]) != "--socket" || std::string_view(argv[3]) != "--fd-socket")
        return 2;
    Mu::IPC::FdChannel fdChannel;
    std::string error;
    if (!fdChannel.connect(argv[4], &error, ::getppid()))
        return 2;
    Mu::Worker::Runtime::WorkerServer server(argv[2], &fdChannel, { }, ::getppid(), { });
    if (!server.listen(&error))
        return 2;
    return server.run(&error);
}
