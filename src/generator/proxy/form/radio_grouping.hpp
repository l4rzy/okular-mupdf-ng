// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_RADIO_GROUPING_HPP
#define MU_GENERATOR_PROXY_FORM_RADIO_GROUPING_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Mu::Generator::Proxy::Form {

/// Minimal identity needed to reconstruct radio groups across document pages.
struct RadioGroupMember {
    int id = 0;
    std::string groupName;
    std::int32_t fieldObjectNumber = -1;
};

inline std::unordered_map<int, std::vector<int>> radioSiblingIds(const std::vector<RadioGroupMember>& members)
{
    // Prefer the explicit group name; fall back to the field object number for
    // documents that omit a group name.
    std::unordered_map<std::string, std::vector<int>> groups;
    for (const auto& member : members) {
        if (!member.groupName.empty())
            groups.try_emplace("name:" + member.groupName).first->second.push_back(member.id);
        else if (member.fieldObjectNumber > 0)
            groups.try_emplace("object:" + std::to_string(member.fieldObjectNumber)).first->second.push_back(member.id);
    }

    std::unordered_map<int, std::vector<int>> siblings;
    // Expose only groups with at least two members and omit each button itself
    // from its sibling list as required by Okular.
    for (const auto& group : groups) {
        const auto& ids = group.second;
        if (ids.size() < 2)
            continue;
        for (int id : ids) {
            auto& siblingIds = siblings[id];
            for (int sibling : ids) {
                if (sibling != id)
                    siblingIds.push_back(sibling);
            }
        }
    }
    return siblings;
}

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_RADIO_GROUPING_HPP
