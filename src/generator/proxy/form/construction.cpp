// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/construction.hpp"

#include "generator/proxy/form/checkbox.hpp"
#include "generator/proxy/form/choice.hpp"
#include "generator/proxy/form/coordinator.hpp"
#include "generator/proxy/form/push_button.hpp"
#include "generator/proxy/form/radio_button.hpp"
#include "generator/proxy/form/radio_grouping.hpp"
#include "generator/proxy/form/text.hpp"

#include <okular/core/page.h>
#include <unordered_map>
#include <utility>

namespace Mu::Generator::Proxy::Form {

QList<Okular::FormField*> createFormFields(std::vector<Model::FormField>& fields, Coordinator& coordinator)
{
    QList<Okular::FormField*> proxies;
    for (auto& field : fields) {
        const auto handle = field.handle;
        const int id = field.pdfObjectNumber;
        IField* proxy = nullptr;
        switch (field.type) {
        case Model::FormFieldType::Text:
            proxy = new Text(id, std::move(field), &coordinator);
            break;
        case Model::FormFieldType::CheckBox:
            proxy = new CheckBox(id, std::move(field), &coordinator);
            break;
        case Model::FormFieldType::RadioButton:
            proxy = new RadioButton(id, std::move(field), &coordinator);
            break;
        case Model::FormFieldType::PushButton:
            proxy = new PushButton(id, std::move(field), &coordinator);
            break;
        case Model::FormFieldType::ComboBox:
        case Model::FormFieldType::ListBox:
            proxy = new Choice(id, std::move(field), &coordinator);
            break;
        }
        if (proxy) {
            coordinator.registerField(handle, proxy);
            proxies.append(proxy->formField());
        }
    }
    return proxies;
}

void assignRadioSiblings(const QVector<Okular::Page*>& pages)
{
    std::vector<RadioGroupMember> members;
    std::unordered_map<int, RadioButton*> proxies;
    for (const auto* page : pages) {
        for (auto* field : page->formFields()) {
            if (auto* radio = dynamic_cast<RadioButton*>(field)) {
                const auto& model = radio->model();
                members.push_back(
                    { radio->id(), model.groupName, model.fieldObjectNumber, model.onState, model.radiosInUnison });
                proxies.emplace(radio->id(), radio);
            }
        }
    }
    // Repeated in-unison options need separate UI groups; canonical worker
    // responses still synchronize every copy of the logical PDF field.
    for (const auto& [id, siblings] : radioSiblingIds(members)) {
        const auto proxy = proxies.find(id);
        if (proxy != proxies.end()) {
            QList<int> siblingIds;
            for (int sibling : siblings)
                siblingIds.append(sibling);
            proxy->second->setSiblings(siblingIds);
        }
    }
}

} // namespace Mu::Generator::Proxy::Form
