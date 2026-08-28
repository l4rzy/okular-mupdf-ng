// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sys/sys.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace Mu::Worker::Sys {

// =============================================================================
// FileDescriptor RAII Implementation
// =============================================================================

FileDescriptor::FileDescriptor(int fd) noexcept
    : m_fd(fd)
{
}

FileDescriptor::~FileDescriptor()
{
    reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : m_fd(std::exchange(other.m_fd, -1))
{
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept
{
    if (this != &other)
        reset(std::exchange(other.m_fd, -1));
    return *this;
}

int FileDescriptor::get() const noexcept
{
    return m_fd;
}

FileDescriptor::operator bool() const noexcept
{
    return m_fd >= 0;
}

int FileDescriptor::release() noexcept
{
    return std::exchange(m_fd, -1);
}

void FileDescriptor::reset(int fd) noexcept
{
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = fd;
}

// =============================================================================
// Shared Memory Mapping RAII Implementation
// =============================================================================

Mapping::Mapping(void* address, std::size_t size) noexcept
    : m_address(address)
    , m_size(size)
{
}

Mapping::~Mapping()
{
    reset();
}

Mapping::Mapping(Mapping&& other) noexcept
    : m_address(std::exchange(other.m_address, MAP_FAILED))
    , m_size(std::exchange(other.m_size, 0))
{
}

Mapping& Mapping::operator=(Mapping&& other) noexcept
{
    if (this != &other) {
        reset();
        m_address = std::exchange(other.m_address, MAP_FAILED);
        m_size = std::exchange(other.m_size, 0);
    }
    return *this;
}

void* Mapping::data() const noexcept
{
    return m_address == MAP_FAILED ? nullptr : m_address;
}

Mapping::operator bool() const noexcept
{
    return data() != nullptr;
}

void Mapping::reset() noexcept
{
    if (m_address != MAP_FAILED)
        ::munmap(m_address, m_size);
    m_address = MAP_FAILED;
    m_size = 0;
}

// =============================================================================
// OS IPC & Memory Allocation Primitives
// =============================================================================

std::optional<FileDescriptor> createEventFd(std::string* error)
{
    // EFD_CLOEXEC ensures the descriptor is not leaked across execve.
    // EFD_NONBLOCK ensures poll/read calls never stall the event loop.
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd >= 0)
        return FileDescriptor(fd);
    if (error)
        *error = std::strerror(errno);
    return std::nullopt;
}

// memfd_create has no libc wrapper on several toolchains, so it goes through
// syscall(2) directly; the anonymous file is pre-sized with ftruncate so the
// peer can map it immediately after the FD transfer over SCM_RIGHTS.
std::optional<FileDescriptor> createMemfd(std::string_view name, std::size_t size, std::string* error)
{
    MU_LOG(debug, "Mu::Worker", std::string("MemFD created, size = ") + std::to_string(size));
    if (name.empty() || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
        if (error)
            *error = "memfd name or size is invalid";
        return std::nullopt;
    }
    // Invoke memfd_create syscall directly
    const int fd = static_cast<int>(::syscall(SYS_memfd_create, name.data(), MFD_CLOEXEC));
    if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int saved = errno;
        if (fd >= 0)
            ::close(fd);
        if (error)
            *error = std::strerror(saved);
        return std::nullopt;
    }
    return FileDescriptor(fd);
}

} // namespace Mu::Worker::Sys
