// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Native Unix-domain descriptor side channel.
 *
 * The binary control plane carries request metadata over CtrlChannel. This
 * channel carries one raw in-memory uint64_t transfer ID and one SCM_RIGHTS
 * descriptor per SOCK_SEQPACKET packet. Both endpoints are local, so the
 * transfer ID does not need a cross-machine byte-order conversion. Descriptors returned by receive() belong to the
 * caller; descriptors passed to send() remain owned by the caller.
 *
 * The listening endpoint authenticates peers using SO_PEERCRED and removes
 * its filesystem socket after a connection is accepted or closed.
 */
#ifndef MU_SHARED_TRANSPORT_FD_CHANNEL_HPP
#define MU_SHARED_TRANSPORT_FD_CHANNEL_HPP

#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "shared/transport/common.hpp"
#include "shared/transport/poll.hpp"

namespace Mu::IPC {

class FdChannel {
public:
    FdChannel() = default;

    ~FdChannel();

    FdChannel(const FdChannel&) = delete;
    FdChannel& operator=(const FdChannel&) = delete;

    /// Creates a private owner-only Unix socket and waits for a peer in accept().
    bool listen(const std::string& path, std::string* error = nullptr);

    /// Accepts and authenticates a peer, rejecting connections from another UID
    /// or an unexpected process. The timeout applies across EINTR/retry loops.
    bool accept(std::string* error = nullptr,
                std::int64_t expectedPeerPid = -1,
                int timeoutMs = Timeout::FdChannelTimeoutMs);

    /// Connects to a Unix socket and verifies the server's UID and optional PID.
    /// The connect operation is synchronous; callers use the bounded send/receive
    /// timeouts for packet operations after the connection is established.
    bool connect(const std::string& path, std::string* error = nullptr, std::int64_t expectedPeerPid = -1);

    /// Sends one transfer ID with exactly one descriptor through SCM_RIGHTS.
    /// SOCK_SEQPACKET preserves packet boundaries; the source descriptor is
    /// borrowed and remains open for the caller to manage.
    bool send(std::uint64_t transferId,
              int descriptor,
              std::string* error = nullptr,
              int timeoutMs = Timeout::FdChannelTimeoutMs) const;

    /// Receives one packet and returns its descriptor to the caller. The packet
    /// is accepted only when it has one descriptor, no truncation flags, and
    /// the expected transfer ID; all descriptors in a rejected packet close.
    int receive(std::uint64_t expectedTransferId,
                std::string* error = nullptr,
                int timeoutMs = Timeout::FdChannelTimeoutMs) const;

    /// Closes the connected/listening socket and removes any listening path.
    void close();

    /// Returns whether a socket descriptor is currently owned by this channel.
    [[nodiscard]] bool valid() const noexcept;

    /// Returns the owned socket descriptor without transferring ownership.
    [[nodiscard]] int fd() const noexcept;

    /// Compatibility alias for valid().
    bool isConnected() const;

    /// Validates that ancillary data contains exactly one SCM_RIGHTS descriptor.
    /// Returns that descriptor without closing it; the caller decides whether to
    /// retain or close it after packet validation.
    static int extractDescriptor(msghdr& msg);

    /// Closes every descriptor received in ancillary data after packet rejection.
    static void closeDescriptors(msghdr& msg);

    /// Stores an operation error when the caller supplied an error destination.
    static bool fail(std::string* error, std::string_view message);

    int m_fd = -1;
    std::string m_path;
};

} // namespace Mu::IPC
#endif // MU_SHARED_TRANSPORT_FD_CHANNEL_HPP
