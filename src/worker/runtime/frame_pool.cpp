// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/frame_pool.hpp"

#include <algorithm>
#include <string>
#include <sys/mman.h>
#include <utility>

#include "shared/protocol/limits.hpp"

namespace Mu::Worker::Runtime {

void* FramePool::Frame::data() const noexcept
{
    return m_slot ? m_slot->mapping.data() : m_allocation.mapping.data();
}

int FramePool::Frame::descriptor() const noexcept
{
    return m_slot ? m_slot->fd.get() : m_allocation.fd.get();
}

std::expected<FramePool::Frame, Model::Error> FramePool::acquireFrame(std::size_t bytes)
{
    if (bytes > Limit::MaxSharedFrameBytes)
        return std::unexpected(
            Model::Error { Model::ErrorCode::ResourceLimit, "render", "frame exceeds transfer limit" });

    Frame frame;
    for (auto& candidate : m_slots) {
        if (!candidate.leased && candidate.capacity >= bytes
            && (!frame.m_slot || candidate.capacity < frame.m_slot->capacity))
            frame.m_slot = &candidate;
    }
    if (frame.m_slot)
        return frame;

    std::string error;
    auto fd = Sys::createMemfd("mupdf-frame", bytes, &error);
    if (!fd)
        return std::unexpected(Model::Error { Model::ErrorCode::ResourceLimit, "render", std::move(error) });
    Sys::Mapping mapping(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd->get(), 0), bytes);
    if (!mapping)
        return std::unexpected(Model::Error { Model::ErrorCode::Internal, "render", "could not map frame" });

    frame.m_reusable = m_slots.size() < Limit::MaxFrameSlotCount && bytes <= Limit::MaxSharedFrameBytes - m_bytes;
    frame.m_allocation = { frame.m_reusable ? m_nextSlotId++ : 0, 0, bytes, std::move(*fd), std::move(mapping), false };
    return frame;
}

FramePool::Lease FramePool::publishFrame(Frame& frame)
{
    if (!frame.m_slot && frame.m_reusable) {
        m_slots.push_back(std::move(frame.m_allocation));
        frame.m_slot = &m_slots.back();
        m_bytes += frame.m_slot->capacity;
    }
    if (!frame.m_slot)
        return { };

    frame.m_slot->leased = true;
    return { frame.m_slot->id, ++frame.m_slot->leaseId };
}

void FramePool::releaseFrame(std::uint64_t slotId, std::uint64_t leaseId) noexcept
{
    const auto slot = std::find_if(
        m_slots.begin(), m_slots.end(), [slotId](const Slot& candidate) { return candidate.id == slotId; });
    if (slot != m_slots.end() && slot->leased && slot->leaseId == leaseId)
        slot->leased = false;
}

void FramePool::clearFrames() noexcept
{
    m_slots.clear();
    m_bytes = 0;
}

} // namespace Mu::Worker::Runtime
