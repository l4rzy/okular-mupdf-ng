// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_RUNTIME_FRAME_POOL_HPP
#define MU_WORKER_RUNTIME_FRAME_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include "shared/model/types.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Runtime {

/// Document-scoped render storage. Used serially: finish or discard an acquired
/// frame before acquiring another or clearing the pool. IPC stays with the caller.
class FramePool {
    struct Slot {
        std::uint64_t id = 0;
        std::uint64_t leaseId = 0;
        std::uint64_t capacity = 0;
        Sys::FileDescriptor fd;
        Sys::Mapping mapping;
        bool leased = false;
    };

public:
    /// New storage is owned here until publication; discarding it rolls back
    /// allocation automatically. Existing idle slots remain owned by the pool.
    class Frame {
    public:
        Frame(Frame&&) noexcept = default;
        Frame& operator=(Frame&&) noexcept = default;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;

        [[nodiscard]] void* data() const noexcept;
        [[nodiscard]] int descriptor() const noexcept;

        [[nodiscard]] bool needsTransfer() const noexcept { return !m_slot; }

    private:
        friend class FramePool;
        Frame() = default;

        Slot* m_slot = nullptr;
        Slot m_allocation;
        bool m_reusable = false;
    };

    struct Lease {
        std::uint64_t slotId = 0;
        std::uint64_t leaseId = 0;
    };

    FramePool() = default;
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    FramePool(FramePool&&) = delete;
    FramePool& operator=(FramePool&&) = delete;

    /// Reuses the smallest fitting idle slot, or allocates new storage. When
    /// either pool limit is full, new storage remains transient.
    [[nodiscard]] std::expected<Frame, Model::Error> acquireFrame(std::size_t bytes);
    /// Called only after rendering and any descriptor transfer have succeeded.
    /// A transient frame has no lease and is freed when Frame is destroyed.
    [[nodiscard]] Lease publishFrame(Frame& frame);
    /// Unknown or stale releases are harmless, including after a document close.
    void releaseFrame(std::uint64_t slotId, std::uint64_t leaseId) noexcept;
    /// Retains the ID sequence so old document leases cannot match new slots.
    void clearFrames() noexcept;

private:
    std::vector<Slot> m_slots;
    std::uint64_t m_bytes = 0;
    std::uint64_t m_nextSlotId = 1;
};

} // namespace Mu::Worker::Runtime

#endif // MU_WORKER_RUNTIME_FRAME_POOL_HPP
