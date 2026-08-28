// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_RADIO_BUTTON_HPP
#define MU_GENERATOR_PROXY_FORM_RADIO_BUTTON_HPP

#include <okular/core/form.h>

#include "generator/proxy/form/coordinator.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Proxy::Form {

/// Okular radio-button view with worker-canonicalized group state.
class RadioButton final : public Okular::FormFieldButton, public IField {
public:
    RadioButton(int id, Model::FormField data, Coordinator* coordinator);
    ~RadioButton() override = default;

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
    QList<int> siblings() const override;
    // Set after all pages are translated because siblings may cross page bounds.
    void setSiblings(const QList<int>& siblings);

    // Applies the worker's checked state after radio-group canonicalization.
    bool applyCanonicalValue(const Model::FormValue& value) override;

    Okular::FormField* formField() override { return this; }

    const Model::FormField& model() const noexcept { return m_data; }

private:
    int m_id;
    // Snapshot of field metadata and canonical checked state.
    Model::FormField m_data;
    // Non-owning coordinator used to submit a checked-state change.
    Coordinator* m_coordinator;
    // Okular IDs of other buttons in this radio group, excluding this button.
    QList<int> m_siblings;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_RADIO_BUTTON_HPP
