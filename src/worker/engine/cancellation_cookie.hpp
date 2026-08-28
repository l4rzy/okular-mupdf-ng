// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_ENGINE_CANCELLATION_COOKIE_HPP
#define MUPDF_WORKER_ENGINE_CANCELLATION_COOKIE_HPP

#include <atomic>

extern "C" {
#include <mupdf/fitz.h>
}

namespace Mu::Worker::Engine {

/**
 * Synchronization wrapper bridging C++ atomic cancellation to MuPDF's `fz_cookie`.
 *
 * Concurrency & ABI Model:
 * 1. `std::atomic_bool m_cancelled` provides standard-conformant acquire/release
 *    cancellation signaling across C++ threads and watchdog timers.
 * 2. MuPDF's C API cooperatively polls `fz_cookie::abort` during rendering
 *    (`fz_run_page`, `fz_new_stext_page_from_page`). MuPDF's design explicitly
 *    expects external threads to asynchronously set `cookie->abort = 1` during
 *    in-flight operations.
 * 3. On modern architectures (x86_64, AArch64), aligned 32-bit integer writes
 *    to `m_cookie.abort` are naturally hardware-atomic.
 */
class CancellationCookie {
public:
    CancellationCookie()
    {
        m_cookie.abort = 0;
        m_cookie.progress = 0;
        m_cookie.progress_max = 0;
        m_cookie.errors = 0;
        m_cookie.incomplete = 0;
    }

    /// Asynchronously requests cancellation of the active operation.
    /// Sets both the C++ atomic token and MuPDF's cooperative `fz_cookie::abort` flag.
    void cancel() noexcept
    {
        m_cancelled.store(true, std::memory_order_release);
        m_cookie.abort = 1;
    }

    /// Thread-safe query checking whether cancellation was previously requested.
    [[nodiscard]] bool isCancelled() const noexcept { return m_cancelled.load(std::memory_order_acquire); }

    /// Synchronizes the atomic cancellation state into the raw `fz_cookie::abort` field.
    void sync() noexcept
    {
        if (m_cancelled.load(std::memory_order_acquire)) {
            m_cookie.abort = 1;
        }
    }

    /// Returns pointer to raw `fz_cookie` structure passed to MuPDF C routines.
    [[nodiscard]] fz_cookie* get() noexcept { return &m_cookie; }

private:
    fz_cookie m_cookie { 0, 0, 0, 0, 0 };
    std::atomic_bool m_cancelled { false };
};

} // namespace Mu::Worker::Engine

#endif // MUPDF_WORKER_ENGINE_CANCELLATION_COOKIE_HPP
