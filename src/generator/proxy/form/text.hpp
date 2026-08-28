// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_TEXT_HPP
#define MU_GENERATOR_PROXY_FORM_TEXT_HPP

#include <okular/core/form.h>

#include "generator/proxy/form/coordinator.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Proxy::Form {

/// Okular text-field view backed by a worker-owned form field.
class Text final : public Okular::FormFieldText, public IField {
public:
    Text(int id, Model::FormField data, Coordinator* coordinator);
    ~Text() override = default;

    Okular::NormalizedRect rect() const override;
    QString name() const override;
    QString uiName() const override;
    QString fullyQualifiedName() const override;
    bool isReadOnly() const override;
    bool isVisible() const override;
    bool isPrintable() const override;

    int id() const override { return m_id; }

    TextType textType() const override;
    QString text() const override;
    void setText(const QString& text) override;
    void setAppearanceText(const QString& text) override;
    bool isPassword() const override;
    bool isRichText() const override;
    int maximumLength() const override;

    // Applies the worker's canonical text after a submitted edit.
    bool applyCanonicalValue(const Model::FormValue& value) override;

    Okular::FormField* formField() override { return this; }

    const Model::FormField& model() const noexcept { return m_data; }

private:
    int m_id;
    // Snapshot of metadata and current canonical text exposed to Okular.
    Model::FormField m_data;
    // Non-owning coordinator used to submit edits to the worker.
    Coordinator* m_coordinator;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_TEXT_HPP
