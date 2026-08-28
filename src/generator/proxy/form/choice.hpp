// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_CHOICE_HPP
#define MU_GENERATOR_PROXY_FORM_CHOICE_HPP

#include <okular/core/form.h>

#include "generator/proxy/form/coordinator.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Proxy::Form {

/// Okular list/combo-box view backed by a worker-owned choice field.
class Choice final : public Okular::FormFieldChoice, public IField {
public:
    Choice(int id, Model::FormField data, Coordinator* coordinator);
    ~Choice() override = default;

    Okular::NormalizedRect rect() const override;
    QString name() const override;
    QString uiName() const override;
    QString fullyQualifiedName() const override;
    bool isReadOnly() const override;
    bool isVisible() const override;
    bool isPrintable() const override;

    int id() const override { return m_id; }

    ChoiceType choiceType() const override;
    QStringList choices() const override;
    bool isEditable() const override;
    bool multiSelect() const override;
    QList<int> currentChoices() const override;
    void setCurrentChoices(const QList<int>& choices) override;
    QString editChoice() const override;
    void setEditChoice(const QString& text) override;
    void setAppearanceChoiceText(const QString& text) override;
    // Applies either the worker's selected indices or canonical custom text.
    bool applyCanonicalValue(const Model::FormValue& value) override;

    Okular::FormField* formField() override { return this; }

    const Model::FormField& model() const noexcept { return m_data; }

private:
    int m_id;
    // Snapshot of choices and current selection used by Okular getters.
    Model::FormField m_data;
    // Non-owning coordinator used to submit selection/text edits.
    Coordinator* m_coordinator;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_CHOICE_HPP
