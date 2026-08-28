// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_SHARED_TRANSPORT_CTRL_CHANNEL_HPP
#define MUPDF_SHARED_TRANSPORT_CTRL_CHANNEL_HPP

/**
 * Linux-only Unix-domain control-plane transport.
 *
 * The channel carries bounded length-prefixed frames over SOCK_STREAM and is
 * deliberately separate from the descriptor-passing side channel. CtrlChannel
 * owns its descriptor and is move-only.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "shared/protocol/limits.hpp"
#include "shared/transport/common.hpp"
#include "shared/transport/poll.hpp"

namespace Mu::IPC {

using Limit::MaxFrameBytes;

enum class ReadStatus {
    /// No complete frame is currently buffered and a nonblocking read found nothing.
    NoData,
    /// A frame header or payload is incomplete.
    Partial,
    /// Exactly one complete frame was removed from the channel buffer.
    Complete,
    /// The peer closed the stream before a complete frame was available.
    Closed,
    /// The stream or frame was malformed, or an I/O/allocation error occurred.
    Error,
};

class CtrlChannel {
public:
    CtrlChannel() = default;
    explicit CtrlChannel(int fd);
    ~CtrlChannel();

    CtrlChannel(const CtrlChannel&) = delete;
    CtrlChannel& operator=(const CtrlChannel&) = delete;
    CtrlChannel(CtrlChannel&& other) noexcept;
    CtrlChannel& operator=(CtrlChannel&& other) noexcept;

    /// Returns the owned socket descriptor without transferring ownership.
    [[nodiscard]] int fd() const;
    /// Returns whether this channel currently owns an open socket descriptor.
    [[nodiscard]] bool valid() const;

    /// Creates a nonblocking owner-only Unix stream listener at `path`.
    bool listen(const std::string& path, std::string* error = nullptr);
    /// Connects with a bounded nonblocking handshake, then authenticates the peer.
    bool connect(const std::string& path,
                 std::int64_t expectedPeerPid,
                 std::string* error = nullptr,
                 int timeoutMs = Timeout::HandshakeMs);
    /// Waits for and authenticates one client, returning the connected channel.
    [[nodiscard]] CtrlChannel
    accept(std::int64_t expectedPeerPid, int timeoutMs, std::string* error = nullptr, IoResult* result = nullptr);
    /// Waits for requested readiness events using one monotonic timeout budget.
    bool wait(short events, int timeoutMs, std::string* error = nullptr) const;
    /// Closes the socket and discards any partially buffered receive state.
    void close();

private:
    int m_fd = -1;
    std::vector<std::byte> m_rxBuffer;
    std::optional<std::uint32_t> m_rxFrameLength;
    bool m_rxPeerClosed = false;

    friend ReadStatus tryReadFrame(CtrlChannel&, std::vector<std::byte>*, std::string*);
};

/// Writes one non-empty length-prefixed frame under a single timeout budget.
bool writeFrame(CtrlChannel& socket, std::span<const std::byte> payload, int timeoutMs, std::string* error = nullptr);
/// Reads one complete frame, blocking between nonblocking parser attempts as needed.
bool readFrame(CtrlChannel& socket, std::vector<std::byte>* payload, int timeoutMs, std::string* error = nullptr);
/// Performs one nonblocking read/parser pass and preserves incomplete bytes in `socket`.
ReadStatus tryReadFrame(CtrlChannel& socket, std::vector<std::byte>* payload, std::string* error = nullptr);

} // namespace Mu::IPC

#endif // MUPDF_SHARED_TRANSPORT_CTRL_CHANNEL_HPP
