// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_SANDBOX_REPORT_HPP
#define MU_SHARED_SANDBOX_REPORT_HPP

#include "shared/model/sandbox_status.hpp"

#include <cstdio>
#include <ostream>

namespace Mu::Model {

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

inline void printSandboxStatusHuman(std::ostream& out, const SandboxStatus& status)
{
    out << "Sandbox: " << status.platform << " profile=" << status.profile
        << " isolation=" << Mu::Model::isolationLevelName(status.isolation()) << "\n";
    for (const auto& protection : status.protections) {
        out << "  " << protection.id << ": " << Mu::Model::protectionStateName(protection.state)
            << " required=" << protection.required << " version=" << protection.version << "\n";
        if (!protection.diagnostic.empty())
            out << "    " << protection.diagnostic << "\n";
    }
    for (const auto& diagnostic : status.diagnostics)
        out << "  " << diagnostic << "\n";
    out.flush();
}

inline void printSandboxStatusJson(std::ostream& out, const SandboxStatus& status)
{
    out << "{\"platform\":\"" << escapeJsonString(status.platform) << "\",\"profile\":\""
        << escapeJsonString(status.profile) << "\",\"isolation\":\""
        << Mu::Model::isolationLevelName(status.isolation()) << '\"'
        << ",\"policySatisfied\":" << (status.isFullyHardened() ? "true" : "false") << ",\"protections\":[";
    bool first = true;
    for (const auto& p : status.protections) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"id\":\"" << escapeJsonString(p.id) << "\",\"category\":\""
            << Mu::Model::protectionCategoryName(p.category) << "\",\"state\":\""
            << Mu::Model::protectionStateName(p.state) << "\",\"required\":" << (p.required ? "true" : "false")
            << ",\"version\":\"" << escapeJsonString(p.version) << "\",\"diagnostic\":\""
            << escapeJsonString(p.diagnostic) << "\"}";
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

} // namespace Mu::Model

#endif // MU_SHARED_SANDBOX_REPORT_HPP
