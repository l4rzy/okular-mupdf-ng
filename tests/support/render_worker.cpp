// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <csignal>
#include <string_view>
#include <unistd.h>

#include "runtime/worker_server.hpp"

extern "C" {
#include <mupdf/fitz.h>

fz_device* __real_fz_new_draw_device(fz_context*, fz_matrix, fz_pixmap*);
}

namespace {

volatile std::sig_atomic_t pauseNextRender = 0;

// The test worker renders one request at a time. Save the armed device's
// callback so the hook forwards directly to MuPDF without a proxy device.
decltype(fz_device::fill_text) fillText = nullptr;
bool drawingPaused = false;

} // namespace

// Test-only linker wrapper. SIGUSR1 arms a stop at the first text drawn.
extern "C" fz_device* __wrap_fz_new_draw_device(fz_context* context, fz_matrix transform, fz_pixmap* pixmap)
{
    auto* device = __real_fz_new_draw_device(context, transform, pixmap);
    if (pauseNextRender) {
        pauseNextRender = 0;
        drawingPaused = false;
        fillText = device->fill_text;
        device->fill_text = [](fz_context* ctx,
                               fz_device* dev,
                               const fz_text* text,
                               fz_matrix ctm,
                               fz_colorspace* colorspace,
                               const float* color,
                               float alpha,
                               fz_color_params params) {
            if (!drawingPaused) {
                drawingPaused = true;
                std::raise(SIGSTOP);
            }
            fillText(ctx, dev, text, ctm, colorspace, color, alpha, params);
        };
    }
    return device;
}

// Use the real command service and renderer, without sandbox activation: these
// tests stop/resume the process, which can require seccomp's restart_syscall.
// This executable is test-only and is never installed.
int main(int argc, char** argv)
{
    if (argc != 5 || std::string_view(argv[1]) != "--socket" || std::string_view(argv[3]) != "--fd-socket")
        return 2;
    struct sigaction action { };
    action.sa_handler = [](int) {
        pauseNextRender = 1;
    };
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGUSR1, &action, nullptr) != 0)
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
