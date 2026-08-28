// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/transport/poll.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <poll.h>

namespace Mu::IPC {

MonotonicDeadline::MonotonicDeadline(std::chrono::milliseconds timeout)
    : m_expiry(std::chrono::steady_clock::now() + timeout)
{
    // steady_clock is not affected by wall-clock corrections, so changing the
    // system time cannot extend or shorten an in-flight I/O operation.
}

MonotonicDeadline MonotonicDeadline::never() noexcept
{
    // An empty expiry is passed to poll as -1, its conventional infinite wait.
    return { };
}

MonotonicDeadline MonotonicDeadline::fromMilliseconds(int ms) noexcept
{
    // Callers use negative timeouts for operations that may wait indefinitely.
    return ms < 0 ? never() : MonotonicDeadline(std::chrono::milliseconds(ms));
}

bool MonotonicDeadline::expired() const noexcept
{
    // An unset deadline represents an infinite wait and therefore never expires.
    return m_expiry && std::chrono::steady_clock::now() >= *m_expiry;
}

int MonotonicDeadline::pollTimeoutMilliseconds() const noexcept
{
    if (!m_expiry)
        return -1;
    // poll accepts milliseconds while the clock calculation may produce a
    // wider duration. Truncate toward zero and clamp to the API's int range.
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(*m_expiry - std::chrono::steady_clock::now()).count();
    return remaining <= 0                             ? 0
        : remaining > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                      : static_cast<int>(remaining);
}

IoResult waitForFd(int fd, short events, MonotonicDeadline& deadline, std::string* error)
{
    if (fd < 0) {
        if (error)
            *error = "descriptor is invalid or not connected";
        return IoResult::Invalid;
    }

    pollfd pfd { fd, events, 0 };
    int result;
    // Reusing the same deadline means signal interruptions cannot reset the
    // timeout budget.
    do
        result = ::poll(&pfd, 1, deadline.pollTimeoutMilliseconds());
    while (result < 0 && errno == EINTR);

    if (result == 0) {
        if (error)
            *error = "operation timed out";
        return IoResult::Timeout;
    }
    if (result < 0) {
        if (error)
            *error = std::strerror(errno);
        return IoResult::Error;
    }

    // A requested event takes precedence. For example, a readable descriptor
    // may also report POLLHUP, and the caller can still drain its final bytes.
    if (pfd.revents & events)
        return IoResult::Complete;
    if (pfd.revents & POLLNVAL) {
        if (error)
            *error = "descriptor is invalid";
        return IoResult::Invalid;
    }
    if (pfd.revents & POLLERR) {
        if (error)
            *error = "descriptor reported an I/O error";
        return IoResult::Error;
    }
    if (error)
        *error = "peer disconnected";
    return IoResult::PeerClosed;
}

bool PollLoop::watch(int fd, short events, Callback callback)
{
    if (fd < 0 || !callback)
        return false;
    // Replacing a watch keeps one callback per descriptor and avoids duplicate
    // pollfd entries in the next cycle.
    for (auto& item : m_watches) {
        if (item.fd == fd) {
            item.events = events;
            item.callback = std::move(callback);
            return true;
        }
    }
    m_watches.push_back({ fd, events, std::move(callback) });
    return true;
}

void PollLoop::unwatch(int fd)
{
    // std::erase_if also handles repeated calls and removes any accidental
    // duplicate left by older callers.
    std::erase_if(m_watches, [fd](const Watch& item) { return item.fd == fd; });
}

int PollLoop::runOnce(MonotonicDeadline& deadline, std::string* error)
{
    // Build a fresh pollfd array so each cycle starts with a clean revents
    // field and reflects the current set of watches.
    std::vector<pollfd> descriptors;
    descriptors.reserve(m_watches.size());
    for (const auto& item : m_watches)
        descriptors.push_back({ item.fd, item.events, 0 });
    if (descriptors.empty())
        return 0;

    int result;
    // EINTR does not consume the shared deadline; retry with the remaining
    // time rather than returning a spurious failure to the event loop.
    do
        result =
            ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), deadline.pollTimeoutMilliseconds());
    while (result < 0 && errno == EINTR);

    if (result <= 0) {
        if (result < 0 && error)
            *error = std::strerror(errno);
        return result;
    }

    // Match readiness results back to the callbacks by descriptor. Watches
    // are not owned here, so callback code remains responsible for closing or
    // retaining its descriptor as appropriate.
    for (const auto& descriptor : descriptors) {
        if (!descriptor.revents)
            continue;
        for (const auto& item : m_watches) {
            if (item.fd == descriptor.fd && item.callback) {
                item.callback(descriptor.revents);
                break;
            }
        }
    }
    return result;
}

} // namespace Mu::IPC
