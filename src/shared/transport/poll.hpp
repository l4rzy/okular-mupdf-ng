// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_SHARED_TRANSPORT_POLL_HPP
#define MUPDF_SHARED_TRANSPORT_POLL_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <poll.h>
#include <string>
#include <vector>

namespace Mu::IPC {

/// Result categories for single-descriptor polling and transfers.
enum class IoResult : std::uint8_t {
    Complete, ///< The requested descriptor event occurred.
    Timeout, ///< The deadline expired before the requested event occurred.
    PeerClosed, ///< The peer closed the connection or otherwise became unavailable.
    Invalid, ///< The descriptor was invalid or not connected.
    Error, ///< The operating system reported an I/O error.
};

/// Deadline helper for poll timeouts based on the monotonic system clock.
///
/// A deadline is shared by all waits belonging to one operation. This keeps
/// retries after EINTR, partial transfers, and multiple descriptors within the
/// original timeout budget.
class MonotonicDeadline {
public:
    /// Creates an unset deadline that never expires.
    MonotonicDeadline() = default;

    /// Creates a deadline relative to the current monotonic clock time.
    explicit MonotonicDeadline(std::chrono::milliseconds timeout);

    /// Returns an unset deadline that waits indefinitely.
    static MonotonicDeadline never() noexcept;

    /// Converts a millisecond timeout; negative values mean no timeout.
    static MonotonicDeadline fromMilliseconds(int ms) noexcept;

    /// Reports whether this deadline is set and has passed.
    [[nodiscard]] bool expired() const noexcept;

    /// Computes remaining milliseconds until expiration (0 if expired, -1 if infinite).
    [[nodiscard]] int pollTimeoutMilliseconds() const noexcept;

private:
    std::optional<std::chrono::steady_clock::time_point> m_expiry;
};

/// Synchronously waits for descriptor readiness until the supplied deadline.
///
/// Retries interruptions from signal delivery and translates poll events into
/// an IoResult. The optional error receives a human-readable failure reason.
IoResult waitForFd(int fd, short events, MonotonicDeadline& deadline, std::string* error = nullptr);

/// Multiplexes non-blocking POSIX descriptors and dispatches readiness callbacks.
///
/// PollLoop stores watches but does not own the descriptors. A watch for an
/// already-registered descriptor is updated in place.
class PollLoop {
public:
    using Callback = std::function<void(short)>;

    /// Registers or updates a descriptor watch with the requested poll events.
    /// Returns false for an invalid descriptor or empty callback.
    bool watch(int fd, short events, Callback callback);

    /// Removes a previously registered descriptor watch, if present.
    void unwatch(int fd);

    /// Executes one poll cycle and invokes callbacks for triggered descriptors.
    ///
    /// Returns the number of descriptors reported by poll, zero when no
    /// descriptor is watched or the deadline expires, and a negative value on
    /// error. The callback receives the descriptor's raw poll revents mask.
    [[nodiscard]] int runOnce(MonotonicDeadline& deadline, std::string* error = nullptr);

private:
    struct Watch {
        int fd;
        short events;
        Callback callback;
    };

    std::vector<Watch> m_watches;
};

} // namespace Mu::IPC

#endif // MUPDF_SHARED_TRANSPORT_POLL_HPP
