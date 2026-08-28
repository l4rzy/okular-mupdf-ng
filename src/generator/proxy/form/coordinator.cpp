// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/coordinator.hpp"

#include "plugin/worker_client.hpp"

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
