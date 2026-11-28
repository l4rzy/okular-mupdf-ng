// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_FORM_BACKEND_HPP
#define MU_SHARED_FORM_BACKEND_HPP

#include "paperbox_model.hpp"

namespace Mu {

/// Minimal backend contract required by generator-side form proxies.
class FormBackend {
public:
    virtual ~FormBackend() = default;

    [[nodiscard]] virtual std::optional<Model::FormUpdateResponse>
    updateForm(const Model::FormUpdateRequest& request) const = 0;
    [[nodiscard]] virtual std::optional<Model::FormUpdateResponse>
    resetForm(const Model::FormResetRequest& request) const = 0;
    [[nodiscard]] virtual std::optional<Model::FormUpdateResponse>
    clickFormButton(const Model::FormButtonClickRequest& request) const = 0;
};

} // namespace Mu

#endif // MU_SHARED_FORM_BACKEND_HPP
