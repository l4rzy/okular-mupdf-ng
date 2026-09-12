// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_COORDINATOR_HPP
#define MU_GENERATOR_PROXY_FORM_COORDINATOR_HPP

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/model/form_backend.hpp"
#include "shared/model/types.hpp"

namespace Okular {

class FormField;

}

namespace Mu::Generator::Proxy::Form {

/// Outcome of applying a worker canonical value to a proxy snapshot.
/// Rejected values touch nothing; unchanged values refresh their widget but
/// stay clean; only changed values dirty the document.
enum class ApplyResult { Rejected, Unchanged, Changed };

/// Interface implemented by a worker-backed Okular form proxy.
class IField {
public:
    virtual ~IField() = default;
    // Applies the worker's canonical value. Identical values report Unchanged
    // so no-op worker responses refresh their widget without dirtying the
    // document; the display text of a nonempty selection is view state and
    // does not count as a change.
    virtual ApplyResult applyCanonicalValue(const Model::FormValue& value) = 0;
    virtual Okular::FormField* formField() = 0;
};

/// Minimal bridge between Okular form proxies and worker-side canonical state.
///
/// Proxies remain lightweight value views. The coordinator sends mutations to
/// the worker, applies the returned affected-field values, and then asks the
/// generator to refresh the corresponding UI objects.
///
/// Ownership and threading contract: the generator owns the coordinator and
/// the backend; Okular owns the proxies through its pages. All calls must
/// happen on Okular's document thread while userMutex() is held — the
/// generator clears registrations under that lock on close, and proxies
/// unregister themselves on destruction so a proxy destroyed without a full
/// clear() never leaves a dangling entry behind.
class Coordinator {
public:
    // changed reports whether any snapshot actually changed. Every accepted
    // value refreshes its widget, but only changed values dirty the document.
    using PageRefreshCallback =
        std::function<void(const std::vector<Okular::FormField*>&, const std::vector<int>&, bool changed)>;

    explicit Coordinator(Model::FormBackend* backend, PageRefreshCallback refreshCallback = nullptr);
    ~Coordinator() = default;

    // Handles are worker field identities; pointers are non-owning proxy views.
    void registerField(const std::string& handle, IField* field);
    // Removes one registration when its proxy is destroyed without a full
    // clear(). Only erases when the mapped pointer matches, so a stale
    // destructor never removes a re-registered proxy that reused the handle.
    void unregisterField(const std::string& handle, const IField* field);
    // Drops registrations when the owning document and its proxies are closed.
    void clear();

    // Disabled coordinators fail closed while the worker has no active document.
    void setAvailable(bool available) noexcept { m_available = available; }

    [[nodiscard]] bool isAvailable() const noexcept { return m_available; }

    // Sends a user edit and applies all fields canonicalized by the worker.
    // Returns true only when at least one proxy view actually changed.
    [[nodiscard]] bool updateField(const std::string& handle, const Model::FormValue& value);
    // Requests a worker-side reset and applies its affected-field response.
    // Returns true only when at least one proxy view actually changed.
    [[nodiscard]] bool resetForm(const std::string& handle);
    // Restores existing proxies from the clean values returned by a reopened worker document.
    void resetFields(const std::vector<Model::FormField>& fields);

private:
    // Updates local proxy views before notifying the generator/UI layer.
    // Returns true only when at least one proxy view actually changed, so
    // empty or unrecognized responses never dirty the document.
    [[nodiscard]] bool applyResponse(const Model::FormUpdateResponse& response);

    // The backend and registered IField instances are owned elsewhere.
    Model::FormBackend* m_backend = nullptr;
    PageRefreshCallback m_refreshCallback;
    std::unordered_map<std::string, IField*> m_fields;
    bool m_available = true;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_COORDINATOR_HPP
