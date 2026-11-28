// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_FORM_BACKEND_HPP
#define MU_PLUGIN_FORM_BACKEND_HPP

#include <paperbox/model/types.hpp>

namespace Mu::Plugin {

/// Minimal backend contract required by generator-side form proxies.
class FormBackend {
public:
    virtual ~FormBackend() = default;

    [[nodiscard]] virtual std::optional<paperbox::model::FormUpdateResponse>
    updateForm(const paperbox::model::FormUpdateRequest& request) const = 0;
    [[nodiscard]] virtual std::optional<paperbox::model::FormUpdateResponse>
    resetForm(const paperbox::model::FormResetRequest& request) const = 0;
    [[nodiscard]] virtual std::optional<paperbox::model::FormUpdateResponse>
    clickFormButton(const paperbox::model::FormButtonClickRequest& request) const = 0;
};

} // namespace Mu::Plugin

#endif // MU_PLUGIN_FORM_BACKEND_HPP
