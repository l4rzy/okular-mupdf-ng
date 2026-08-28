// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_COORDINATOR_HPP
#define MU_GENERATOR_PROXY_FORM_COORDINATOR_HPP

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/model/types.hpp"

namespace Okular {

class FormField;

}

namespace Mu::Plugin {

class WorkerClient;

}

namespace Mu::Generator::Proxy::Form {

/// Interface implemented by a worker-backed Okular form proxy.
class IField {
public:
    virtual ~IField() = default;
    virtual bool applyCanonicalValue(const Model::FormValue& value) = 0;
    virtual Okular::FormField* formField() = 0;
};

/// Minimal bridge between Okular form proxies and worker-side canonical state.
///
/// Proxies remain lightweight value views. The coordinator sends mutations to
/// the worker, applies the returned affected-field values, and then asks the
/// generator to refresh the corresponding UI objects.
class Coordinator {
public:
    using PageRefreshCallback = std::function<void(const std::vector<Okular::FormField*>&, const std::vector<int>&)>;

    explicit Coordinator(Plugin::WorkerClient* client, PageRefreshCallback refreshCallback = nullptr);
    ~Coordinator() = default;

    // Handles are worker field identities; pointers are non-owning proxy views.
    void registerField(const std::string& handle, IField* field);
    // Drops registrations when the owning document and its proxies are closed.
    void clear();

    // Disabled coordinators fail closed while the worker has no active document.
    void setAvailable(bool available) noexcept { m_available = available; }

    // Sends a user edit and applies all fields canonicalized by the worker.
    [[nodiscard]] bool updateField(const std::string& handle, const Model::FormValue& value);
    // Requests a worker-side reset and applies its affected-field response.
    [[nodiscard]] bool resetForm(const std::string& handle);

private:
    // Updates local proxy views before notifying the generator/UI layer.
    [[nodiscard]] bool applyResponse(const Model::FormUpdateResponse& response);

    // WorkerClient and registered IField instances are owned elsewhere.
    Plugin::WorkerClient* m_client = nullptr;
    PageRefreshCallback m_refreshCallback;
    std::unordered_map<std::string, IField*> m_fields;
    bool m_available = true;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_COORDINATOR_HPP
