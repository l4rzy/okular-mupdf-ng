// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_SANDBOX_REPORT_HPP
#define MU_SHARED_SANDBOX_REPORT_HPP

#include <paperbox/model/sandbox_status.hpp>

#include <cstdio>
#include <ostream>
#include <string_view>

namespace Mu::Cli {

using paperbox::model::ProtectionCategory;
using paperbox::model::ProtectionState;

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

[[nodiscard]] inline IsolationLevel isolation(const paperbox::model::SandboxStatus& status) noexcept
{
    if (status.platform.empty() || status.profile.empty())
        return IsolationLevel::Unknown;
    if (status.isFullyHardened())
        return IsolationLevel::Full;
    if (status.isPartiallyActive())
        return IsolationLevel::Degraded;
    if (status.protections.empty()
        || std::any_of(status.protections.begin(), status.protections.end(), [](const auto& p) {
               return p.required && p.state == ProtectionState::Unknown;
           }))
        return IsolationLevel::Unknown;
    return IsolationLevel::None;
}

inline std::string escapeJsonString(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
        case '\"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else
                out += c;
        }
    }
    return out;
}

inline void printSandboxStatusHuman(std::ostream& out, const paperbox::model::SandboxStatus& status)
{
    out << "Sandbox: " << status.platform << " profile=" << status.profile
        << " isolation=" << isolationLevelName(isolation(status)) << "\n";
    for (const auto& protection : status.protections) {
        out << "  " << protection.id << ": " << protectionStateName(protection.state)
            << " required=" << protection.required << " version=" << protection.version << "\n";
        if (!protection.diagnostic.empty())
            out << "    " << protection.diagnostic << "\n";
    }
    for (const auto& diagnostic : status.diagnostics)
        out << "  " << diagnostic << "\n";
    out.flush();
}

inline void printSandboxStatusJson(std::ostream& out, const paperbox::model::SandboxStatus& status)
{
    out << "{\"platform\":\"" << escapeJsonString(status.platform) << "\",\"profile\":\""
        << escapeJsonString(status.profile) << "\",\"isolation\":\"" << isolationLevelName(isolation(status)) << '\"'
        << ",\"policySatisfied\":" << (status.isFullyHardened() ? "true" : "false") << ",\"protections\":[";
    bool first = true;
    for (const auto& p : status.protections) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"id\":\"" << escapeJsonString(p.id) << "\",\"category\":\"" << protectionCategoryName(p.category)
            << "\",\"state\":\"" << protectionStateName(p.state)
            << "\",\"required\":" << (p.required ? "true" : "false") << ",\"version\":\"" << escapeJsonString(p.version)
            << "\",\"diagnostic\":\"" << escapeJsonString(p.diagnostic) << "\"}";
    }
    out << "],\"diagnostics\":[";
    first = true;
    for (const auto& diagnostic : status.diagnostics) {
        if (!first)
            out << ',';
        first = false;
        out << '\"' << escapeJsonString(diagnostic) << '\"';
    }
    out << "]}\n";
    out.flush();
}

} // namespace Mu::Cli

#endif // MU_SHARED_SANDBOX_REPORT_HPP
