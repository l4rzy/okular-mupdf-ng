// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_MODEL_SANDBOX_STATUS_HPP
#define MU_SHARED_MODEL_SANDBOX_STATUS_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Mu::Model {

enum class ProtectionCategory : std::uint8_t {
    Filesystem,
    Network,
    Syscall,
    Process,
    Resources,
    Memory,
    Transport,
    Other
};
enum class ProtectionState : std::uint8_t { Unknown, Enforced, Unavailable, Disabled, Failed };
enum class IsolationLevel : std::uint8_t { Unknown, Full, Degraded, None };

[[nodiscard]] constexpr std::string_view protectionStateName(ProtectionState state) noexcept
{
    switch (state) {
    case ProtectionState::Unknown:
        return "unknown";
    case ProtectionState::Enforced:
        return "enforced";
    case ProtectionState::Unavailable:
        return "unavailable";
    case ProtectionState::Disabled:
        return "disabled";
    case ProtectionState::Failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view protectionCategoryName(ProtectionCategory category) noexcept
{
    switch (category) {
    case ProtectionCategory::Filesystem:
        return "filesystem";
    case ProtectionCategory::Network:
        return "network";
    case ProtectionCategory::Syscall:
        return "syscall";
    case ProtectionCategory::Process:
        return "process";
    case ProtectionCategory::Resources:
        return "resources";
    case ProtectionCategory::Memory:
        return "memory";
    case ProtectionCategory::Transport:
        return "transport";
    case ProtectionCategory::Other:
        return "other";
    }
    return "other";
}

[[nodiscard]] constexpr std::string_view isolationLevelName(IsolationLevel level) noexcept
{
    switch (level) {
    case IsolationLevel::Unknown:
        return "unknown";
    case IsolationLevel::Full:
        return "full";
    case IsolationLevel::Degraded:
        return "degraded";
    case IsolationLevel::None:
        return "none";
    }
    return "unknown";
}

struct ProtectionStatus {
    std::string id; // Open platform-qualified ID, never a cross-platform enum.
    ProtectionCategory category = ProtectionCategory::Other;
    ProtectionState state = ProtectionState::Unknown;
    bool required = false;
    std::string version, diagnostic;
};

struct SandboxStatus {
    std::string platform, profile;
    std::vector<ProtectionStatus> protections;
    std::vector<std::string> diagnostics;

    // An empty/unnamed profile can never accidentally pass a strict policy.
    [[nodiscard]] bool isFullyHardened() const noexcept
    {
        return !platform.empty() && !profile.empty()
            && std::any_of(protections.begin(), protections.end(), [](const auto& p) { return p.required; })
            && std::all_of(protections.begin(), protections.end(), [](const auto& p) {
                   return !p.required || (!p.id.empty() && p.state == ProtectionState::Enforced);
               });
    }

    [[nodiscard]] bool isPartiallyActive() const noexcept
    {
        return !isFullyHardened() && std::any_of(protections.begin(), protections.end(), [](const auto& p) {
            return p.required && p.state == ProtectionState::Enforced;
        });
    }

    [[nodiscard]] IsolationLevel isolation() const noexcept
    {
        if (platform.empty() || profile.empty())
            return IsolationLevel::Unknown;
        if (isFullyHardened())
            return IsolationLevel::Full;
        if (isPartiallyActive())
            return IsolationLevel::Degraded;
        if (protections.empty() || std::any_of(protections.begin(), protections.end(), [](const auto& p) {
                return p.required && p.state == ProtectionState::Unknown;
            }))
            return IsolationLevel::Unknown;
        return IsolationLevel::None;
    }
};

} // namespace Mu::Model
#endif
