// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_WORKER_SYS_SYS_HPP
#define MU_WORKER_SYS_SYS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <sys/mman.h>

#include "shared/logging.hpp"

namespace Mu::Worker::Sys {

/**
 * RAII wrapper managing the lifecycle of an open POSIX file descriptor.
 *
 * Guarantees automated closure on destruction and prevents descriptor leaks.
 * Non-copyable, move-only.
 */
class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) noexcept;
    ~FileDescriptor();
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) noexcept;
    FileDescriptor& operator=(FileDescriptor&&) noexcept;

    /// Returns the underlying raw file descriptor without transferring ownership.
    [[nodiscard]] int get() const noexcept;

    /// Returns true if this instance holds a valid, non-negative file descriptor.
    [[nodiscard]] explicit operator bool() const noexcept;

    /// Releases ownership and returns the descriptor without closing it.
    [[nodiscard]] int release() noexcept;

    /// Closes current descriptor (if open) and takes ownership of `fd`.
    void reset(int fd = -1) noexcept;

private:
    int m_fd = -1;
};

/**
 * RAII wrapper managing an anonymous shared memory virtual address mapping (mmap).
 *
 * Guarantees automated munmap on destruction. Non-copyable, move-only.
 */
class Mapping {
public:
    Mapping() = default;
    Mapping(void* address, std::size_t size) noexcept;
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&&) noexcept;
    Mapping& operator=(Mapping&&) noexcept;

    /// Returns pointer to the mapped virtual address space, or nullptr if invalid.
    [[nodiscard]] void* data() const noexcept;

    /// Returns true if the memory mapping is active and valid.
    [[nodiscard]] explicit operator bool() const noexcept;

    /// Unmaps the memory region and resets to an unmapped state.
    void reset() noexcept;

private:
    void* m_address = MAP_FAILED;
    std::size_t m_size = 0;
};

/// Creates a non-blocking Linux eventfd descriptor (EFD_NONBLOCK | EFD_CLOEXEC).
std::optional<FileDescriptor> createEventFd(std::string* error = nullptr);

/// Creates an anonymous in-memory file descriptor (memfd_create with MFD_CLOEXEC) sized to `size` bytes.
std::optional<FileDescriptor> createMemfd(std::string_view name, std::size_t size, std::string* error = nullptr);

} // namespace Mu::Worker::Sys
#endif // MU_WORKER_SYS_SYS_HPP
