// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/choice.hpp"

#include <algorithm>

#include "generator/proxy/form/coordinator.hpp"

namespace Mu::Generator::Proxy::Form {

namespace {

QMap<QString, QString> exportValueMap(const Model::FormField& field)
{
    // Only pair entries that exist in both arrays; malformed documents must
    // not make an out-of-range export-value access.
    QMap<QString, QString> result;
    const std::size_t count = std::min(field.choices.size(), field.exportValues.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!field.exportValues[i].empty())
            result.insert(QString::fromStdString(field.choices[i]), QString::fromStdString(field.exportValues[i]));
    }
    return result;
}

} // namespace

Choice::Choice(int id, Model::FormField data, Coordinator* coordinator)
    : m_id(id)
    , m_data(std::move(data))
    , m_coordinator(coordinator)
{
    // Okular uses this map when it serializes a selected display value back to
    // the PDF-facing export value.
    setExportValues(exportValueMap(m_data));
}

Okular::NormalizedRect Choice::rect() const
{
    return Okular::NormalizedRect(
        m_data.rectangle.left, m_data.rectangle.top, m_data.rectangle.right, m_data.rectangle.bottom);
}

QString Choice::name() const
{
    return QString::fromStdString(m_data.partialName);
}

QString Choice::uiName() const
{
    return QString::fromStdString(m_data.uiName.empty() ? m_data.partialName : m_data.uiName);
}

QString Choice::fullyQualifiedName() const
{
    return QString::fromStdString(m_data.fullyQualifiedName.empty() ? m_data.partialName : m_data.fullyQualifiedName);
}

bool Choice::isReadOnly() const
{
    return m_data.readOnly;
}

bool Choice::isVisible() const
{
    return m_data.visible;
}

bool Choice::isPrintable() const
{
    return m_data.printable;
}

Okular::FormFieldChoice::ChoiceType Choice::choiceType() const
{
    return (m_data.type == Model::FormFieldType::ListBox) ? ListBox : ComboBox;
}

QStringList Choice::choices() const
{
    QStringList list;
    list.reserve(static_cast<qsizetype>(m_data.choices.size()));
    for (const auto& item : m_data.choices)
        list.append(QString::fromStdString(item));
    return list;
}

bool Choice::isEditable() const
{
    return m_data.editableCombo;
}

bool Choice::multiSelect() const
{
    return m_data.multiSelect;
}

QList<int> Choice::currentChoices() const
{
    QList<int> list;
    list.reserve(static_cast<qsizetype>(m_data.currentChoices.size()));
    for (int idx : m_data.currentChoices)
        list.append(idx);
    return list;
}

void Choice::setCurrentChoices(const QList<int>& choices)
{
    // Selection changes are sent as a batch so multi-select fields remain
    // consistent when the worker applies field dependencies.
    if (m_data.readOnly)
        return;

    std::vector<int> indices;
    indices.reserve(static_cast<std::size_t>(choices.size()));
    for (int idx : choices)
        indices.push_back(idx);

    if (m_coordinator)
        static_cast<void>(m_coordinator->updateField(m_data.handle, Model::FormChoiceSelection { indices }));
}

QString Choice::editChoice() const
{
    return QString::fromStdString(m_data.text);
}

void Choice::setEditChoice(const QString& text)
{
    // Custom text is valid only for editable combo boxes; read-only fields and
    // list boxes must retain their worker-defined selection.
    if (m_data.readOnly || !m_data.editableCombo)
        return;

    if (m_coordinator)
        static_cast<void>(
            m_coordinator->updateField(m_data.handle, Model::FormChoiceCustomText { text.toStdString() }));
}

void Choice::setAppearanceChoiceText(const QString& text)
{
    m_data.text = text.toStdString();
}

bool Choice::applyCanonicalValue(const Model::FormValue& value)
{
    // Selection and custom text are mutually exclusive canonical states.
    if (const auto* sel = std::get_if<Model::FormChoiceSelection>(&value)) {
        m_data.currentChoices = sel->selectedIndices;
        m_data.text.clear();
        return true;
    } else if (const auto* cust = std::get_if<Model::FormChoiceCustomText>(&value)) {
        m_data.text = cust->text;
        m_data.currentChoices.clear();
        return true;
    }
    return false;
}

} // namespace Mu::Generator::Proxy::Form
