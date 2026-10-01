// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_PUSH_BUTTON_HPP
#define MU_GENERATOR_PROXY_FORM_PUSH_BUTTON_HPP

#include <memory>

#include <okular/core/form.h>

#include "generator/proxy/form/coordinator.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Proxy::Form {

/// Okular push-button view for worker-side form actions.
class PushButton final : public Okular::FormFieldButton, public IField {
public:
    PushButton(int id, Model::FormField data, Coordinator* coordinator);
    ~PushButton() override;

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
    ApplyResult applyCanonicalValue(const Model::FormValue&) override { return ApplyResult::Rejected; }

    Okular::FormField* formField() override { return this; }

    void setHandle(const std::string& handle) override;
    void setPushButtonAction(Model::FormPushButtonAction action) override;

    const Model::FormField& model() const noexcept { return m_data; }

private:
    void updateActivationAction();

    int m_id;
    // Button metadata remains local because button activation is stateless.
    Model::FormField m_data;
    // Shared with Okular's activation action and updated when recovery rekeys the field.
    std::shared_ptr<ButtonActivation> m_activation;
    // Non-owning coordinator used by direct state changes.
    Coordinator* m_coordinator;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_PUSH_BUTTON_HPP
