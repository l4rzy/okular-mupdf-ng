// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/transport/ctrl_channel.hpp"

#include "shared/protocol/limits.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace Mu::IPC {

namespace {

bool makeAddress(const std::string& path, sockaddr_un& address, std::string* error)
{
    // sockaddr_un has a fixed-size pathname field; reject invalid paths before
    // copying so the address is always NUL-terminated.
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        if (error)
            *error = "control socket path is invalid or too long";
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

bool verifyPeer(int fd, std::int64_t expectedPeerPid, std::string* error)
{
    // SO_PEERCRED is kernel-supplied for local sockets and cannot be spoofed by
    // the connecting process. UID matching is mandatory; PID matching is used
    // when the caller knows the expected worker/plugin process.
    ucred credentials { };
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        if (error)
            *error = std::strerror(errno);
        return false;
    }
    if (credentials.uid != ::geteuid() || (expectedPeerPid >= 0 && credentials.pid != expectedPeerPid)) {
        if (error)
            *error = "control socket peer identity did not match the expected process";
        return false;
    }
    return true;
}

std::array<std::byte, 4> encodeFrameLength(std::size_t length)
{
    // Control frames use a four-byte network-order length independent of the
    // host ABI. The caller validates the range before writing the header.
    return {
        static_cast<std::byte>((length >> 24) & 0xff),
        static_cast<std::byte>((length >> 16) & 0xff),
        static_cast<std::byte>((length >> 8) & 0xff),
        static_cast<std::byte>(length & 0xff),
    };
}

bool decodeFrameLength(std::span<const std::byte> header, std::uint32_t* length, std::string* error)
{
    if (header.size() != 4 || !length) {
        if (error)
            *error = "control frame header is invalid";
        return false;
    }
    // Decode the same big-endian representation emitted by encodeFrameLength.
    *length = (std::to_integer<std::uint32_t>(header[0]) << 24) | (std::to_integer<std::uint32_t>(header[1]) << 16)
        | (std::to_integer<std::uint32_t>(header[2]) << 8) | std::to_integer<std::uint32_t>(header[3]);
    if (!*length || *length > Limit::MaxControlMessageBytes) {
        if (error)
            *error = "control frame has an invalid length";
        return false;
    }
    return true;
}

template <typename Byte, typename Transfer>
IoResult transferExact(const CtrlChannel& socket,
                       std::span<Byte> bytes,
                       short events,
                       MonotonicDeadline& deadline,
                       std::size_t& transferred,
                       std::string* error,
                       Transfer transfer)
{
    // SOCK_STREAM may accept or return fewer bytes than requested. Repeatedly
    // transfer the remaining span while the shared monotonic deadline permits.
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const IoResult readiness = waitForFd(socket.fd(), events, deadline, error);
        if (readiness != IoResult::Complete)
            return readiness;
        const auto chunk = bytes.subspan(offset);
        const ssize_t count = transfer(socket.fd(), chunk);
        if (count <= 0) {
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (deadline.expired()) {
                    if (error)
                        *error = "control socket operation timed out";
                    return IoResult::Timeout;
                }
                continue;
            }
            if (error)
                *error = count < 0 ? std::strerror(errno) : "control socket peer disconnected";
            return count < 0 ? IoResult::Error : IoResult::PeerClosed;
        }
        offset += static_cast<std::size_t>(count);
        transferred += static_cast<std::size_t>(count);
    }
    return IoResult::Complete;
}

IoResult writeAll(const CtrlChannel& socket,
                  std::span<const std::byte> bytes,
                  MonotonicDeadline& deadline,
                  std::size_t& transferred,
                  std::string* error)
{
    return transferExact(socket, bytes, POLLOUT, deadline, transferred, error, [](int fd, auto chunk) {
        return ::send(fd, chunk.data(), chunk.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    });
}

} // namespace

CtrlChannel::CtrlChannel(int fd)
    : m_fd(fd)
{
}

CtrlChannel::~CtrlChannel()
{
    close();
}

CtrlChannel::CtrlChannel(CtrlChannel&& other) noexcept
    : m_fd(std::exchange(other.m_fd, -1))
    , m_rxBuffer(std::move(other.m_rxBuffer))
    , m_rxFrameLength(std::exchange(other.m_rxFrameLength, std::nullopt))
    , m_rxPeerClosed(std::exchange(other.m_rxPeerClosed, false))
{
}

CtrlChannel& CtrlChannel::operator=(CtrlChannel&& other) noexcept
{
    if (this != &other) {
        close();
        m_fd = std::exchange(other.m_fd, -1);
        m_rxBuffer = std::move(other.m_rxBuffer);
        m_rxFrameLength = std::exchange(other.m_rxFrameLength, std::nullopt);
        m_rxPeerClosed = std::exchange(other.m_rxPeerClosed, false);
    }
    return *this;
}

int CtrlChannel::fd() const
{
    return m_fd;
}

bool CtrlChannel::valid() const
{
    return m_fd >= 0;
}

bool CtrlChannel::listen(const std::string& path, std::string* error)
{
    // The listener remains nonblocking so accept and event-loop callers can use
    // the same readiness/deadline machinery as frame transfers.
    close();
    sockaddr_un address { };
    if (!makeAddress(path, address, error))
        return false;
    m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0) {
        if (error)
            *error = std::strerror(errno);
        return false;
    }
    if (::fcntl(m_fd, F_SETFL, O_NONBLOCK) != 0) {
        const std::string why = std::strerror(errno);
        close();
        if (error)
            *error = why;
        return false;
    }
    if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string why = std::strerror(errno);
        close();
        if (error)
            *error = why;
        return false;
    }
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0 || ::listen(m_fd, 1) != 0) {
        const std::string why = std::strerror(errno);
        close();
        if (error)
            *error = why;
        return false;
    }
    return true;
}

bool CtrlChannel::connect(const std::string& path, std::int64_t expectedPeerPid, std::string* error, int timeoutMs)
{
    // Temporarily make connect nonblocking so a stalled peer cannot exceed the
    // handshake deadline, then restore the descriptor's original flags before
    // authenticating and handing it to the caller.
    close();
    sockaddr_un address { };
    if (!makeAddress(path, address, error))
        return false;
    m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0) {
        if (error)
            *error = std::strerror(errno);
        return false;
    }
    const int originalFlags = ::fcntl(m_fd, F_GETFL);
    if (originalFlags < 0) {
        const std::string why = std::strerror(errno);
        close();
        if (error)
            *error = why;
        return false;
    }
    if (::fcntl(m_fd, F_SETFL, originalFlags | O_NONBLOCK) != 0) {
        const std::string why = std::strerror(errno);
        close();
        if (error)
            *error = why;
        return false;
    }
    const bool connected = ::connect(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    if (!connected && errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
        const std::string why = std::strerror(errno);
        close();
        if (error)
            *error = why;
        return false;
    }
    if (!connected) {
        MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
        if (waitForFd(m_fd, POLLOUT, deadline, error) != IoResult::Complete) {
            close();
            return false;
        }
        int socketError = 0;
        socklen_t length = sizeof(socketError);
        if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError != 0) {
            const int saved = socketError != 0 ? socketError : errno;
            close();
            if (error)
                *error = std::strerror(saved);
            return false;
        }
    }
    if (::fcntl(m_fd, F_SETFL, originalFlags) != 0) {
        const std::string why = std::strerror(errno);
        close();
        if (error)
            *error = why;
        return false;
    }
    if (!verifyPeer(m_fd, expectedPeerPid, error)) {
        close();
        return false;
    }
    return true;
}

CtrlChannel CtrlChannel::accept(std::int64_t expectedPeerPid, int timeoutMs, std::string* error, IoResult* result)
{
    // Rejected clients do not consume the listener: keep accepting until the
    // deadline expires or a peer with the expected credentials arrives.
    MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
    for (;;) {
        const IoResult waitResult = waitForFd(m_fd, POLLIN, deadline, error);
        if (result)
            *result = waitResult;
        if (waitResult != IoResult::Complete)
            return { };
        const int accepted = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (accepted >= 0) {
            std::string identityError;
            if (!verifyPeer(accepted, expectedPeerPid, &identityError)) {
                ::close(accepted);
                continue;
            }
            if (result)
                *result = IoResult::Complete;
            return CtrlChannel(accepted);
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        if (error)
            *error = std::strerror(errno);
        if (result)
            *result = IoResult::Error;
        return { };
    }
}

bool CtrlChannel::wait(short events, int timeoutMs, std::string* error) const
{
    MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
    return waitForFd(m_fd, events, deadline, error) == IoResult::Complete;
}

void CtrlChannel::close()
{
    // Receive state belongs to the descriptor. Reset it together with the FD so
    // a later listen/connect cannot interpret bytes from the previous peer.
    if (m_fd >= 0)
        ::close(std::exchange(m_fd, -1));
    m_rxBuffer.clear();
    m_rxFrameLength.reset();
    m_rxPeerClosed = false;
}

bool writeFrame(CtrlChannel& socket, std::span<const std::byte> payload, int timeoutMs, std::string* error)
{
    if (payload.empty() || payload.size() > Limit::MaxControlMessageBytes) {
        if (error)
            *error = "control frame is empty or too large";
        return false;
    }
    // Write header and payload as one logical operation. A partial header or
    // payload makes the stream boundary ambiguous and poisons the channel.
    const auto header = encodeFrameLength(payload.size());
    MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
    std::size_t transferred = 0;
    const IoResult headerResult = writeAll(socket, header, deadline, transferred, error);
    if (headerResult != IoResult::Complete) {
        if (headerResult != IoResult::Timeout || transferred)
            socket.close();
        return false;
    }
    if (writeAll(socket, payload, deadline, transferred, error) != IoResult::Complete) {
        socket.close();
        return false;
    }
    return true;
}

bool readFrame(CtrlChannel& socket, std::vector<std::byte>* payload, int timeoutMs, std::string* error)
{
    if (!payload) {
        if (error)
            *error = "control frame output is null";
        return false;
    }
    // tryReadFrame owns the persistent stream parser; this wrapper only waits
    // for more bytes until one frame completes or the deadline is exhausted.
    MonotonicDeadline deadline = MonotonicDeadline::fromMilliseconds(timeoutMs);
    for (;;) {
        const ReadStatus status = tryReadFrame(socket, payload, error);
        if (status == ReadStatus::Complete)
            return true;
        if (status == ReadStatus::Closed || status == ReadStatus::Error)
            return false;
        if (deadline.expired()) {
            if (error)
                *error = "control socket operation timed out";
            if (status == ReadStatus::Partial)
                socket.close();
            return false;
        }
        if (waitForFd(socket.fd(), POLLIN, deadline, error) != IoResult::Complete) {
            if (status == ReadStatus::Partial)
                socket.close();
            return false;
        }
    }
}

ReadStatus tryReadFrame(CtrlChannel& socket, std::vector<std::byte>* payload, std::string* error)
{
    if (!payload || socket.fd() < 0) {
        if (error)
            *error = payload ? "control socket is not connected" : "control frame output is null";
        return ReadStatus::Error;
    }
    // The parser consumes at most one frame per call. Any following bytes stay
    // in m_rxBuffer for the next call, while an incomplete header/payload stays
    // there until the stream provides more data.
    payload->clear();

    const auto consumeBuffered = [&]() -> ReadStatus {
        // First decode a pending length, then wait until exactly that many
        // payload bytes are available. A larger buffer contains the next frame.
        if (!socket.m_rxFrameLength) {
            if (socket.m_rxBuffer.size() < sizeof(std::uint32_t)) {
                if (socket.m_rxPeerClosed) {
                    if (error)
                        *error = "control socket closed during a frame";
                    socket.close();
                    return ReadStatus::Closed;
                }
                return socket.m_rxBuffer.empty() ? ReadStatus::NoData : ReadStatus::Partial;
            }
            std::uint32_t length = 0;
            if (!decodeFrameLength(
                    std::span<const std::byte>(socket.m_rxBuffer.data(), sizeof(std::uint32_t)), &length, error)) {
                socket.close();
                return ReadStatus::Error;
            }
            socket.m_rxBuffer.erase(socket.m_rxBuffer.begin(), socket.m_rxBuffer.begin() + sizeof(std::uint32_t));
            try {
                socket.m_rxBuffer.reserve(length);
            } catch (const std::exception& exception) {
                if (error)
                    *error = std::string("control frame allocation failed: ") + exception.what();
                socket.close();
                return ReadStatus::Error;
            }
            socket.m_rxFrameLength = length;
        }

        if (socket.m_rxBuffer.size() < *socket.m_rxFrameLength) {
            if (socket.m_rxPeerClosed) {
                if (error)
                    *error = "control socket closed during a frame";
                socket.close();
                return ReadStatus::Closed;
            }
            return ReadStatus::Partial;
        }

        if (socket.m_rxBuffer.size() == *socket.m_rxFrameLength) {
            *payload = std::move(socket.m_rxBuffer);
        } else {
            try {
                payload->assign(socket.m_rxBuffer.begin(), socket.m_rxBuffer.begin() + *socket.m_rxFrameLength);
            } catch (const std::exception& exception) {
                if (error)
                    *error = std::string("control frame allocation failed: ") + exception.what();
                socket.close();
                return ReadStatus::Error;
            }
            socket.m_rxBuffer.erase(socket.m_rxBuffer.begin(), socket.m_rxBuffer.begin() + *socket.m_rxFrameLength);
        }
        socket.m_rxFrameLength.reset();
        if (socket.m_rxBuffer.empty())
            std::vector<std::byte>().swap(socket.m_rxBuffer);
        return ReadStatus::Complete;
    };

    const ReadStatus buffered = consumeBuffered();
    if (buffered == ReadStatus::Complete || buffered == ReadStatus::Closed || buffered == ReadStatus::Error)
        return buffered;

    // Probe without blocking; readFrame() performs the blocking wait when it
    // needs to turn NoData or Partial into a completed frame.
    pollfd readiness { socket.fd(), POLLIN | POLLERR | POLLHUP | POLLNVAL, 0 };
    const int ready = ::poll(&readiness, 1, 0);
    if (ready < 0) {
        if (errno == EINTR)
            return ReadStatus::NoData;
        if (error)
            *error = std::strerror(errno);
        socket.close();
        return ReadStatus::Error;
    }
    if (ready == 0)
        return buffered;
    if (readiness.revents & POLLNVAL) {
        if (error)
            *error = "control socket descriptor is invalid";
        socket.close();
        return ReadStatus::Error;
    }
    if ((readiness.revents & POLLERR) && !(readiness.revents & POLLIN)) {
        if (error)
            *error = "control socket reported an I/O error";
        socket.close();
        return ReadStatus::Error;
    }
    if (readiness.revents & POLLHUP)
        socket.m_rxPeerClosed = true;

    // Read in bounded chunks so an adversarial frame cannot force one oversized
    // temporary allocation before its declared length has been validated.
    std::array<std::byte, 64 * 1024> chunk { };
    for (;;) {
        const std::size_t bufferedBytes = socket.m_rxBuffer.size();
        const std::size_t requiredBytes = socket.m_rxFrameLength
            ? static_cast<std::size_t>(*socket.m_rxFrameLength) - bufferedBytes
            : sizeof(std::uint32_t) - bufferedBytes;
        const std::size_t readSize = std::min(chunk.size(), requiredBytes);
        const ssize_t count = ::recv(socket.fd(), chunk.data(), readSize, MSG_DONTWAIT);
        if (count > 0) {
            try {
                socket.m_rxBuffer.insert(socket.m_rxBuffer.end(), chunk.begin(), chunk.begin() + count);
            } catch (const std::exception& exception) {
                if (error)
                    *error = std::string("control frame allocation failed: ") + exception.what();
                socket.close();
                return ReadStatus::Error;
            }
            const bool peerClosed = socket.m_rxPeerClosed;
            socket.m_rxPeerClosed = false;
            const ReadStatus status = consumeBuffered();
            if (status == ReadStatus::Partial || status == ReadStatus::NoData)
                socket.m_rxPeerClosed = peerClosed;
            if (status == ReadStatus::Complete || status == ReadStatus::Closed || status == ReadStatus::Error)
                return status;
            continue;
        }
        if (count == 0) {
            socket.m_rxPeerClosed = true;
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        if (error)
            *error = std::strerror(errno);
        socket.close();
        return ReadStatus::Error;
    }

    return consumeBuffered();
}

} // namespace Mu::IPC
