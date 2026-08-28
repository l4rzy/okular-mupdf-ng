// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_PUSH_BUTTON_HPP
#define MU_GENERATOR_PROXY_FORM_PUSH_BUTTON_HPP

#include <okular/core/form.h>

#include "generator/proxy/form/coordinator.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Proxy::Form {

/// Okular push-button view for worker-side form actions.
class PushButton final : public Okular::FormFieldButton, public IField {
public:
    PushButton(int id, Model::FormField data, Coordinator* coordinator);
    ~PushButton() override = default;

    Okular::NormalizedRect rect() const override;
    QString name() const override;
    QString uiName() const override;
    QString fullyQualifiedName() const override;
    bool isReadOnly() const override;
    bool isVisible() const override;
    bool isPrintable() const override;

    int id() const override { return m_id; }

    ButtonType buttonType() const override;
    QString caption() const override;
    bool state() const override;
    void setState(bool state) override;

    QList<int> siblings() const override { return { }; }

    // Push buttons have no persistent value to apply from an update response.
    bool applyCanonicalValue(const Model::FormValue&) override { return false; }

    Okular::FormField* formField() override { return this; }

    const Model::FormField& model() const noexcept { return m_data; }

private:
    int m_id;
    // Button metadata remains local because button activation is stateless.
    Model::FormField m_data;
    // Non-owning coordinator used for reset-form actions.
    Coordinator* m_coordinator;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_PUSH_BUTTON_HPP
