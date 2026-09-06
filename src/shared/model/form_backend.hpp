// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_MODEL_FORM_BACKEND_HPP
#define MU_SHARED_MODEL_FORM_BACKEND_HPP

#include "shared/model/types.hpp"

namespace Mu::Model {

/// Minimal backend contract required by generator-side form proxies.
class FormBackend {
public:
    virtual ~FormBackend() = default;

    [[nodiscard]] virtual std::optional<FormUpdateResponse> updateForm(const FormUpdateRequest& request) const = 0;
    [[nodiscard]] virtual std::optional<FormUpdateResponse> resetForm(const FormResetRequest& request) const = 0;
};

} // namespace Mu::Model

#endif // MU_SHARED_MODEL_FORM_BACKEND_HPP
