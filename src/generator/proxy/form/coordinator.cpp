// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/coordinator.hpp"

#include <algorithm>
#include <optional>

#include "plugin/worker_client.hpp"

namespace {

std::optional<Mu::Model::FormValue> formValue(const Mu::Model::FormField& field)
{
    using namespace Mu::Model;
    switch (field.type) {
    case FormFieldType::Text:
        return FormTextValue { field.text };
    case FormFieldType::CheckBox:
    case FormFieldType::RadioButton:
        return FormCheckValue { field.checked };
    case FormFieldType::ComboBox:
    case FormFieldType::ListBox:
        if (!field.currentChoices.empty())
            return FormChoiceSelection { field.currentChoices };
        if (field.type == FormFieldType::ListBox || !field.editableCombo)
            return FormChoiceSelection { };
        return FormChoiceCustomText { field.text };
    case FormFieldType::PushButton:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

namespace Mu::Generator::Proxy::Form {

Coordinator::Coordinator(Plugin::WorkerClient* client, PageRefreshCallback refreshCallback)
    : m_client(client)
    , m_refreshCallback(std::move(refreshCallback))
{
}

void Coordinator::registerField(const std::string& handle, IField* field)
{
    // Ignore incomplete registrations; a missing handle cannot be addressed by
    // a worker response and a null proxy cannot be refreshed safely.
    if (!handle.empty() && field)
        m_fields[handle] = field;
}

void Coordinator::clear()
{
    // Proxies are destroyed with the document, so never retain their addresses.
    m_fields.clear();
}

bool Coordinator::updateField(const std::string& handle, const Model::FormValue& value)
{
    // Availability is controlled by the generator around document/restart
    // transitions; reject edits before they reach a stale worker session.
    if (!m_available || handle.empty())
        return false;

    if (!m_client)
        return false;

    const auto response = m_client->updateForm({ handle, value });
    if (!response)
        return false;

    return applyResponse(*response);
}

bool Coordinator::resetForm(const std::string& handle)
{
    if (!m_available || handle.empty() || !m_client)
        return false;

    const auto response = m_client->resetForm({ handle });
    return response && applyResponse(*response);
}

void Coordinator::resetFields(const std::vector<Model::FormField>& fields)
{
    std::vector<Okular::FormField*> changedFields;
    std::vector<int> affectedPages;
    for (const auto& field : fields) {
        const auto value = formValue(field);
        const auto it = m_fields.find(field.handle);
        if (!value || it == m_fields.end() || !it->second || !it->second->applyCanonicalValue(*value))
            continue;

        changedFields.push_back(it->second->formField());
        if (std::find(affectedPages.begin(), affectedPages.end(), field.page) == affectedPages.end())
            affectedPages.push_back(field.page);
    }

    if (m_refreshCallback)
        m_refreshCallback(changedFields, affectedPages);
}

bool Coordinator::applyResponse(const Model::FormUpdateResponse& response)
{
    // The worker may canonicalize the requested value and update dependent
    // fields, so consume every affected field rather than only the origin.
    std::vector<Okular::FormField*> changedFields;
    changedFields.reserve(response.affectedFields.size());
    for (const auto& fieldState : response.affectedFields) {
        auto it = m_fields.find(fieldState.handle);
        if (it != m_fields.end() && it->second && it->second->applyCanonicalValue(fieldState.value))
            changedFields.push_back(it->second->formField());
    }

    if (m_refreshCallback)
        m_refreshCallback(changedFields, response.affectedPages);

    return true;
}

} // namespace Mu::Generator::Proxy::Form
