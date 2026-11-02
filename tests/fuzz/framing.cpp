// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/protocol/limits.hpp"
#include "shared/transport/ctrl_channel.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

// Compare each nonblocking read against a small model of the bytes sent so far.
// The model retains the length prefix until the entire frame is available.
bool drain(Mu::IPC::CtrlChannel& channel, std::vector<std::byte>& pending, bool closed)
{
    using Mu::IPC::ReadStatus;
    for (;;) {
        ReadStatus expected = pending.empty() ? ReadStatus::NoData : ReadStatus::Partial;
        std::size_t length = 0;
        if (pending.size() >= 4) {
            for (std::size_t i = 0; i < 4; ++i)
                length = (length << 8) | std::to_integer<unsigned>(pending[i]);
            if (length == 0 || length > Mu::Limit::MaxControlMessageBytes)
                expected = ReadStatus::Error;
            else if (length <= pending.size() - 4)
                expected = ReadStatus::Complete;
        }
        if (closed && (expected == ReadStatus::NoData || expected == ReadStatus::Partial))
            expected = ReadStatus::Closed;

        std::vector<std::byte> payload;
        const auto actual = Mu::IPC::tryReadFrame(channel, &payload, nullptr);
        if (actual != expected)
            std::abort();
        if (actual == ReadStatus::Complete) {
            if (!std::equal(payload.begin(), payload.end(), pending.begin() + 4, pending.begin() + 4 + length))
                std::abort();
            pending.erase(pending.begin(), pending.begin() + 4 + length);
            continue;
        }
        if (!payload.empty())
            std::abort();
        if ((actual == ReadStatus::Error || actual == ReadStatus::Closed) && channel.valid())
            std::abort();
        return actual == ReadStatus::Error || actual == ReadStatus::Closed;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > 65537)
        return 0;
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, descriptors) != 0)
        std::abort();
    Mu::IPC::CtrlChannel reader(descriptors[0]);
    const int writer = descriptors[1];
    std::vector<std::byte> pending;
    const auto bytes = std::as_bytes(std::span(data + 1, size - 1));
    std::size_t offset = 0;
    bool terminal = drain(reader, pending, false);
    while (offset < bytes.size() && !terminal) {
        const std::size_t count = std::min<std::size_t>(1 + data[offset % size] % 128, bytes.size() - offset);
        ssize_t written;
        do {
            written = ::send(writer, bytes.data() + offset, count, MSG_DONTWAIT | MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        if (written <= 0)
            std::abort();
        const auto sent = static_cast<std::size_t>(written);
        pending.insert(pending.end(), bytes.begin() + offset, bytes.begin() + offset + sent);
        offset += sent;
        // Sometimes close while bytes are still queued, exercising POLLHUP
        // together with readable data rather than only an already-drained EOF.
        if (offset < bytes.size() || !(data[0] & 128))
            terminal = drain(reader, pending, false);
    }
    if (!terminal) {
        if (::shutdown(writer, SHUT_WR) != 0)
            std::abort();
        if (!drain(reader, pending, true))
            std::abort();
    }
    ::close(writer);
    return 0;
}
