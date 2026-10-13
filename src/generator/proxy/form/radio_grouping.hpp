// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_RADIO_GROUPING_HPP
#define MU_GENERATOR_PROXY_FORM_RADIO_GROUPING_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Mu::Generator::Proxy::Form {

/// Minimal identity needed to reconstruct UI radio groups.
struct RadioGroupMember {
    int id = 0;
    std::string groupName;
    std::int32_t fieldObjectNumber = -1;
    int page = -1;
    std::string onState { };
};

inline std::unordered_map<int, std::vector<int>> radioSiblingIds(const std::vector<RadioGroupMember>& members)
{
    // Preserve logical groups across pages unless they contain mirrored options:
    // Qt exclusivity cannot represent several copies of the selected option.
    std::unordered_map<std::string, std::vector<const RadioGroupMember*>> groups;
    for (const auto& member : members) {
        if (!member.groupName.empty())
            groups["name:" + member.groupName].push_back(&member);
        else if (member.fieldObjectNumber > 0)
            groups["object:" + std::to_string(member.fieldObjectNumber)].push_back(&member);
    }

    std::unordered_map<int, std::vector<int>> siblings;
    for (const auto& [key, group] : groups) {
        std::unordered_map<std::string, int> optionPages;
        bool mirrored = false;
        for (const auto* member : group) {
            if (member->onState.empty())
                continue;
            const auto [it, inserted] = optionPages.emplace(member->onState, member->page);
            if (!inserted && it->second != member->page)
                mirrored = true;
        }
        // The worker still synchronizes the logical field across every page.
        std::unordered_map<int, std::vector<int>> uiGroups;
        for (const auto* member : group)
            uiGroups[mirrored ? member->page : -1].push_back(member->id);
        for (const auto& [page, ids] : uiGroups) {
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
    }
    return siblings;
}

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_RADIO_GROUPING_HPP
