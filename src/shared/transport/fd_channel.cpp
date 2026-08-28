// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/transport/fd_channel.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace Mu::IPC {

FdChannel::~FdChannel()
{
    close();
}

bool FdChannel::listen(const std::string& path, std::string* error)
{
    close();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
        return fail(error, "FD socket path is too long");
    // Remove only the requested stale endpoint before bind; close() handles the
    // channel's own path during later teardown.
    ::unlink(path.c_str());
    m_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (m_fd < 0)
        return fail(error, std::strerror(errno));
    sockaddr_un address { };
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    // The endpoint is private to the current user; SOCK_CLOEXEC prevents it from
    // leaking into unrelated exec'ed processes.
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
        || ::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0 || ::listen(m_fd, 1) != 0) {
        const std::string why = std::strerror(errno);
        close();
        ::unlink(path.c_str());
        return fail(error, why);
    }
    m_path = path;
    return true;
}

bool FdChannel::accept(std::string* error, std::int64_t expectedPeerPid, int timeoutMs)
{
    // The listening socket is replaced by the authenticated connected socket.
    // Once accepted, unlink the pathname so no later process can discover it.
    MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
    for (;;) {
        if (waitForFd(m_fd, POLLIN, deadline, error) != IoResult::Complete)
            return false;
        const int fd = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return fail(error, std::strerror(errno));
        }
        ucred credentials { };
        socklen_t len = sizeof(credentials);
        const bool trusted = ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &len) == 0
            && credentials.uid == ::geteuid() && (expectedPeerPid < 0 || credentials.pid == expectedPeerPid);
        if (!trusted) {
            // Authentication failures are discarded without exposing their
            // descriptor or allowing them to consume the accepted session.
            ::close(fd);
            continue;
        }
        ::close(m_fd);
        m_fd = fd;
        if (!m_path.empty())
            ::unlink(m_path.c_str());
        m_path.clear();
        return true;
    }
}

bool FdChannel::connect(const std::string& path, std::string* error, std::int64_t expectedPeerPid)
{
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
        return fail(error, "FD socket path is too long");
    close();
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fail(error, std::strerror(errno));
    sockaddr_un address { };
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string why = std::strerror(errno);
        ::close(fd);
        return fail(error, why);
    }
    // Authenticate after connect because SO_PEERCRED is available on the
    // connected local socket; close() also clears the descriptor on failure.
    m_fd = fd;
    ucred credentials { };
    socklen_t len = sizeof(credentials);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &len) != 0 || credentials.uid != ::geteuid()
        || (expectedPeerPid >= 0 && credentials.pid != expectedPeerPid)) {
        close();
        return fail(error, "FD socket peer identity did not match the expected process");
    }
    return true;
}

bool FdChannel::send(std::uint64_t transferId, int descriptor, std::string* error, int timeoutMs) const
{
    if (m_fd < 0 || descriptor < 0)
        return fail(error, "FD channel is not connected");
    // The packet has exactly one fixed-size transfer ID plus one SCM_RIGHTS
    // control message. SOCK_SEQPACKET makes partial packet delivery invalid.
    char data[sizeof(transferId)];
    std::memcpy(data, &transferId, sizeof(transferId));
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] { };
    iovec iov { data, sizeof(data) };
    msghdr msg { };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &descriptor, sizeof(descriptor));
    MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
    for (;;) {
        if (waitForFd(m_fd, POLLOUT, deadline, error) != IoResult::Complete)
            return false;
        const ssize_t written = ::sendmsg(m_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written == ssize_t(sizeof(data)))
            return true;
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return fail(error, written < 0 ? std::strerror(errno) : "FD channel sent a partial packet");
    }
}

int FdChannel::receive(std::uint64_t expectedTransferId, std::string* error, int timeoutMs) const
{
    if (m_fd < 0) {
        fail(error, "FD channel is not connected");
        return -1;
    }
    MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
    for (;;) {
        if (waitForFd(m_fd, POLLIN, deadline, error) != IoResult::Complete)
            return -1;

        // The receive buffer allows several ancillary descriptors so malformed
        // packets can be rejected and every received descriptor can be closed.
        std::uint64_t transferId = 0;
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 16)> control { };
        iovec iov { &transferId, sizeof(transferId) };
        msghdr msg { };
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();
        const ssize_t read = ::recvmsg(m_fd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (read < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (read < 0) {
            fail(error, std::strerror(errno));
            return -1;
        }
        const int descriptor = extractDescriptor(msg);
        // Validate payload size, ancillary completeness, and correlation before
        // returning ownership of any descriptor to the caller.
        const bool packetValid = read == ssize_t(sizeof(transferId)) && !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
            && transferId == expectedTransferId && descriptor >= 0;
        if (packetValid)
            return descriptor;
        closeDescriptors(msg);
        fail(error, "invalid FD channel packet");
        return -1;
    }
}

void FdChannel::close()
{
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = -1;
    if (!m_path.empty()) {
        ::unlink(m_path.c_str());
        m_path.clear();
    }
}

bool FdChannel::valid() const noexcept
{
    return m_fd >= 0;
}

int FdChannel::fd() const noexcept
{
    return m_fd;
}

bool FdChannel::isConnected() const
{
    return m_fd >= 0;
}

int FdChannel::extractDescriptor(msghdr& msg)
{
    // Count all SCM_RIGHTS entries, including entries from multiple control
    // messages. Exactly one descriptor is the only acceptable packet shape.
    int descriptor = -1;
    int rightsCount = 0;
    bool malformed = false;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0)) {
            malformed = true;
            continue;
        }
        const std::size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0)
            malformed = true;
        const auto* descriptors = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        for (std::size_t index = 0; index < bytes / sizeof(int); ++index) {
            ++rightsCount;
            if (rightsCount == 1)
                descriptor = descriptors[index];
        }
    }
    return !malformed && rightsCount == 1 ? descriptor : -1;
}

void FdChannel::closeDescriptors(msghdr& msg)
{
    // recvmsg installs received descriptors in this process even when the
    // packet is malformed; close every SCM_RIGHTS entry to avoid FD leaks.
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0))
            continue;
        const std::size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        const auto* descriptors = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        for (std::size_t index = 0; index < bytes / sizeof(int); ++index) {
            const int descriptor = descriptors[index];
            if (descriptor >= 0)
                ::close(descriptor);
        }
    }
}

bool FdChannel::fail(std::string* error, std::string_view message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace Mu::IPC
