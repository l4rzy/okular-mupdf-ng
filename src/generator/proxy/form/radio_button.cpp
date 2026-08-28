// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/radio_button.hpp"

#include "generator/proxy/form/coordinator.hpp"

namespace Mu::Generator::Proxy::Form {

RadioButton::RadioButton(int id, Model::FormField data, Coordinator* coordinator)
    : m_id(id)
    , m_data(std::move(data))
    , m_coordinator(coordinator)
{
}

Okular::NormalizedRect RadioButton::rect() const
{
    return Okular::NormalizedRect(
        m_data.rectangle.left, m_data.rectangle.top, m_data.rectangle.right, m_data.rectangle.bottom);
}

QString RadioButton::name() const
{
    return QString::fromStdString(m_data.partialName);
}

QString RadioButton::uiName() const
{
    return QString::fromStdString(m_data.uiName.empty() ? m_data.partialName : m_data.uiName);
}

QString RadioButton::fullyQualifiedName() const
{
    return QString::fromStdString(m_data.fullyQualifiedName.empty() ? m_data.partialName : m_data.fullyQualifiedName);
}

bool RadioButton::isReadOnly() const
{
    return m_data.readOnly;
}

bool RadioButton::isVisible() const
{
    return m_data.visible;
}

bool RadioButton::isPrintable() const
{
    return m_data.printable;
}

Okular::FormFieldButton::ButtonType RadioButton::buttonType() const
{
    return Radio;
}

QString RadioButton::caption() const
{
    return QString::fromStdString(m_data.uiName.empty() ? m_data.partialName : m_data.uiName);
}

bool RadioButton::state() const
{
    return m_data.checked;
}

void RadioButton::setState(bool state)
{
    // A no-toggle-to-off radio must remain selected once checked; all other
    // group changes are canonicalized by the worker response.
    if (m_data.readOnly || m_data.checked == state)
        return;

    if (!state && m_data.noToggleToOff) {
        return;
    }

    if (m_coordinator && !m_data.handle.empty())
        static_cast<void>(m_coordinator->updateField(m_data.handle, Model::FormCheckValue { state }));
}

bool RadioButton::applyCanonicalValue(const Model::FormValue& value)
{
    if (const auto* cv = std::get_if<Model::FormCheckValue>(&value)) {
        m_data.checked = cv->checked;
        return true;
    }
    return false;
}

QList<int> RadioButton::siblings() const
{
    return m_siblings;
}

void RadioButton::setSiblings(const QList<int>& siblings)
{
    m_siblings = siblings;
}

} // namespace Mu::Generator::Proxy::Form
