// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_WINDOW_MENUS_HPP
#define MU_GENERATOR_WINDOW_MENUS_HPP

#include <QList>
#include <QObject>
#include <QPointer>

#include <optional>

class QAction;
class KXMLGUIClient;
class KXMLGUIFactory;

namespace Mu::Generator {

class WindowMenus;

// Okular registers generator GUI clients independently of their tabs. Keep one
// presentation per factory while retaining each document's actions and state.
class MenuRegistration final : public QObject {
public:
    explicit MenuRegistration(KXMLGUIClient* client);
    ~MenuRegistration() override;
    void schedule();
    int nextOcrMode(int currentMode);

private:
    void attach();
    void detach();
    KXMLGUIClient* m_client;
    std::optional<QPointer<QObject>> m_owner;
    QPointer<KXMLGUIFactory> m_factory;
    QPointer<WindowMenus> m_menus;
    QMetaObject::Connection m_removed;
    QList<QAction*> m_configurable;
    bool m_pending = false;
    std::optional<int> m_previousOcrMode;
};

} // namespace Mu::Generator

#endif
