// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/push_button.hpp"

#include <okular/core/action.h>

namespace Mu::Generator::Proxy::Form {

PushButton::PushButton(int id, Model::FormField data, Coordinator* coordinator)
    : m_id(id)
    , m_data(std::move(data))
    , m_coordinator(coordinator)
{
    if (m_data.pushButtonAction == Model::FormPushButtonAction::Reset
        || m_data.pushButtonAction == Model::FormPushButtonAction::JavaScript) {
        m_activation = std::make_shared<ButtonActivation>(ButtonActivation { m_data.pushButtonAction, m_data.handle });
        auto* action = new Okular::BackendOpaqueAction;
        action->setNativeHandle(m_activation);
        setActivationAction(action);
    }
}

PushButton::~PushButton()
{
    if (m_coordinator)
        m_coordinator->unregisterField(m_data.handle, this);
}

Okular::NormalizedRect PushButton::rect() const
{
    return Okular::NormalizedRect(
        m_data.rectangle.left, m_data.rectangle.top, m_data.rectangle.right, m_data.rectangle.bottom);
}

QString PushButton::name() const
{
    return QString::fromStdString(m_data.partialName);
}

QString PushButton::uiName() const
{
    return QString::fromStdString(m_data.uiName.empty() ? m_data.partialName : m_data.uiName);
}

QString PushButton::fullyQualifiedName() const
{
    return QString::fromStdString(m_data.fullyQualifiedName.empty() ? m_data.partialName : m_data.fullyQualifiedName);
}

bool PushButton::isReadOnly() const
{
    return m_data.readOnly;
}

bool PushButton::isVisible() const
{
    return m_data.visible;
}

bool PushButton::isPrintable() const
{
    return m_data.printable;
}

Okular::FormFieldButton::ButtonType PushButton::buttonType() const
{
    return Okular::FormFieldButton::Push;
}

QString PushButton::caption() const
{
    return QString::fromStdString(m_data.buttonCaption);
}

bool PushButton::state() const
{
    return false;
}

void PushButton::setState(bool state)
{
    if (!state || m_data.readOnly || !m_coordinator)
        return;

    if (m_data.pushButtonAction == Model::FormPushButtonAction::Reset)
        static_cast<void>(m_coordinator->resetForm(m_data.handle));
    else if (m_data.pushButtonAction == Model::FormPushButtonAction::JavaScript)
        static_cast<void>(m_coordinator->clickFormButton(m_data.handle));
}

void PushButton::setHandle(const std::string& handle)
{
    m_data.handle = handle;
    if (m_activation)
        m_activation->handle = handle;
}

} // namespace Mu::Generator::Proxy::Form
