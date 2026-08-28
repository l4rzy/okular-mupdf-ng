// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_CHECKBOX_HPP
#define MU_GENERATOR_PROXY_FORM_CHECKBOX_HPP

#include <okular/core/form.h>

#include "generator/proxy/form/coordinator.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Proxy::Form {

/// Okular checkbox view backed by a worker-owned form field.
class CheckBox final : public Okular::FormFieldButton, public IField {
public:
    CheckBox(int id, Model::FormField data, Coordinator* coordinator);
    ~CheckBox() override = default;

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

    // Applies the worker's canonical value after a local edit or dependency
    // update; the proxy does not optimistically mutate checked state.
    bool applyCanonicalValue(const Model::FormValue& value) override;

    Okular::FormField* formField() override { return this; }

    const Model::FormField& model() const noexcept { return m_data; }

private:
    int m_id;
    // Snapshot of field metadata and current canonical value for Okular reads.
    Model::FormField m_data;
    // Non-owning coordinator used to submit edits to the worker.
    Coordinator* m_coordinator;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_CHECKBOX_HPP
