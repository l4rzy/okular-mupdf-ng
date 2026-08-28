#include "generator/proxy/form/text.hpp"

#include "generator/proxy/form/coordinator.hpp"

namespace Mu::Generator::Proxy::Form {

Text::Text(int id, Model::FormField data, Coordinator* coordinator)
    : m_id(id)
    , m_data(std::move(data))
    , m_coordinator(coordinator)
{
}

Okular::NormalizedRect Text::rect() const
{
    return Okular::NormalizedRect(
        m_data.rectangle.left, m_data.rectangle.top, m_data.rectangle.right, m_data.rectangle.bottom);
}

QString Text::name() const
{
    return QString::fromStdString(m_data.partialName);
}

QString Text::uiName() const
{
    return QString::fromStdString(m_data.uiName.empty() ? m_data.partialName : m_data.uiName);
}

QString Text::fullyQualifiedName() const
{
    return QString::fromStdString(m_data.fullyQualifiedName.empty() ? m_data.partialName : m_data.fullyQualifiedName);
}

bool Text::isReadOnly() const
{
    return m_data.readOnly;
}

bool Text::isVisible() const
{
    return m_data.visible;
}

bool Text::isPrintable() const
{
    return m_data.printable;
}

Okular::FormFieldText::TextType Text::textType() const
{
    return m_data.multiline ? Multiline : Normal;
}

QString Text::text() const
{
    return QString::fromStdString(m_data.text);
}

void Text::setText(const QString& text)
{
    // Do not update optimistically: the worker may normalize the value or
    // update other fields before returning the canonical response.
    if (m_data.readOnly || m_data.text == text.toStdString())
        return;

    if (m_coordinator)
        static_cast<void>(m_coordinator->updateField(m_data.handle, Model::FormTextValue { text.toStdString() }));
}

void Text::setAppearanceText(const QString& text)
{
    m_data.text = text.toStdString();
}

bool Text::applyCanonicalValue(const Model::FormValue& value)
{
    if (const auto* tv = std::get_if<Model::FormTextValue>(&value)) {
        m_data.text = tv->text;
        return true;
    }
    return false;
}

bool Text::isPassword() const
{
    return m_data.password;
}

bool Text::isRichText() const
{
    return false;
}

int Text::maximumLength() const
{
    return m_data.maximumLength <= 0 ? -1 : m_data.maximumLength;
}

} // namespace Mu::Generator::Proxy::Form
