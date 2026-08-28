// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/checkbox.hpp"

#include "generator/proxy/form/coordinator.hpp"

namespace Mu::Generator::Proxy::Form {

CheckBox::CheckBox(int id, Model::FormField data, Coordinator* coordinator)
    : m_id(id)
    , m_data(std::move(data))
    , m_coordinator(coordinator)
{
}

Okular::NormalizedRect CheckBox::rect() const
{
    return Okular::NormalizedRect(
        m_data.rectangle.left, m_data.rectangle.top, m_data.rectangle.right, m_data.rectangle.bottom);
}

QString CheckBox::name() const
{
    return QString::fromStdString(m_data.partialName);
}

QString CheckBox::uiName() const
{
    return QString::fromStdString(m_data.uiName.empty() ? m_data.partialName : m_data.uiName);
}

QString CheckBox::fullyQualifiedName() const
{
    return QString::fromStdString(m_data.fullyQualifiedName.empty() ? m_data.partialName : m_data.fullyQualifiedName);
}

bool CheckBox::isReadOnly() const
{
    return m_data.readOnly;
}

bool CheckBox::isVisible() const
{
    return m_data.visible;
}

bool CheckBox::isPrintable() const
{
    return m_data.printable;
}

Okular::FormFieldButton::ButtonType CheckBox::buttonType() const
{
    return Okular::FormFieldButton::CheckBox;
}

QString CheckBox::caption() const
{
    return QString::fromStdString(m_data.uiName.empty() ? m_data.partialName : m_data.uiName);
}

bool CheckBox::state() const
{
    return m_data.checked;
}

void CheckBox::setState(bool state)
{
    // Let the worker validate and canonicalize the edit before changing the
    // local view; this also updates dependent fields through the response.
    if (m_data.readOnly || m_data.checked == state)
        return;

    if (m_coordinator)
        static_cast<void>(m_coordinator->updateField(m_data.handle, Model::FormCheckValue { state }));
}

bool CheckBox::applyCanonicalValue(const Model::FormValue& value)
{
    if (const auto* cv = std::get_if<Model::FormCheckValue>(&value)) {
        m_data.checked = cv->checked;
        return true;
    }
    return false;
}

} // namespace Mu::Generator::Proxy::Form
