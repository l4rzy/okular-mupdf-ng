// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_CLI_SANDBOX_REPORT_HPP
#define MU_CLI_SANDBOX_REPORT_HPP

#include <paperbox/model/sandbox_status.hpp>

#include <algorithm>
#include <cstdio>
#include <ostream>
#include <string_view>

namespace Mu::Cli {

inline std::string_view protectionStateName(paperbox::model::ProtectionState state)
{
    using enum paperbox::model::ProtectionState;
    switch (state) {
    case Unknown:
        return "unknown";
    case Enforced:
        return "enforced";
    case Unavailable:
        return "unavailable";
    case Disabled:
        return "disabled";
    case Failed:
        return "failed";
    }
    return "unknown";
}

inline std::string_view protectionCategoryName(paperbox::model::ProtectionCategory category)
{
    using enum paperbox::model::ProtectionCategory;
    switch (category) {
    case Filesystem:
        return "filesystem";
    case Network:
        return "network";
    case Syscall:
        return "syscall";
    case Process:
        return "process";
    case Resources:
        return "resources";
    case Memory:
        return "memory";
    case Transport:
        return "transport";
    case Other:
        return "other";
    }
    return "other";
}

inline std::string_view isolationName(const paperbox::model::SandboxStatus& status)
{
    // Keep these labels consistent between the text and JSON reports.
    if (status.platform.empty() || status.profile.empty())
        return "unknown";
    if (status.isFullyHardened())
        return "full";
    if (status.isPartiallyActive())
        return "degraded";
    if (status.protections.empty() || std::ranges::any_of(status.protections, [](const auto& protection) {
            return protection.required && protection.state == paperbox::model::ProtectionState::Unknown;
        }))
        return "unknown";
    return "none";
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
    out << "Sandbox: " << status.platform << " profile=" << status.profile << " isolation=" << isolationName(status)
        << "\n";
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
        << escapeJsonString(status.profile) << "\",\"isolation\":\"" << isolationName(status) << '\"'
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

#endif // MU_CLI_SANDBOX_REPORT_HPP
