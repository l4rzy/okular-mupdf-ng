// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/window_menus.hpp"
#include "mupdfngsettings.h"

#include <KActionCollection>
#include <KActionMenu>
#include <KLocalizedString>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <KXmlGuiWindow>
#include <QAction>
#include <QActionGroup>
#include <QHash>
#include <QMenu>
#include <QTimer>
#include <okular/interfaces/viewerinterface.h>

namespace Mu::Generator {

class WindowMenus final : public QObject, public KXMLGUIClient {
public:
    explicit WindowMenus(KXMLGUIFactory* factory)
        : QObject(factory)
    {
        setComponentName(QStringLiteral("okular_mupdfng"), i18n("MuPDF"));
        connect(factory, &KXMLGUIFactory::makingChanges, this, [this, factory](bool changing) {
            if (!changing || m_rebuildPending || !this->factory())
                return;
            // Unplug before the owning part is removed; otherwise XMLGUI
            // leaves Tools alive without an owner and cannot remove it later.
            m_rebuildPending = true;
            // Queue first: clientRemoved callbacks may destroy this controller.
            QTimer::singleShot(0, this, [this, factory] {
                m_rebuildPending = false;
                factory->addClient(this);
            });
            factory->removeClient(this);
        });
        connect(factory, &KXMLGUIFactory::clientAdded, this, &WindowMenus::refreshActions);
        connect(factory, &KXMLGUIFactory::clientRemoved, this, [this](KXMLGUIClient* client) {
            if (client != this)
                refreshActions();
        });
    }

    void addClient(KXMLGUIClient* client, const std::optional<QPointer<QObject>>& owner)
    {
        m_clients.append(Client { client, owner });
        const auto actions = client->KXMLGUIClient::actionCollection()->actions();
        if (m_clients.size() == 1) {
            QHash<QActionGroup*, QActionGroup*> groups;
            for (QAction* source : actions) {
                QAction* action = source->menu() ? new KActionMenu(source->icon(), source->text(), this)
                                                 : new QAction(source->icon(), source->text(), this);
                action->setCheckable(source->isCheckable());
                if (auto* sourceGroup = source->actionGroup()) {
                    auto*& group = groups[sourceGroup];
                    if (!group) {
                        group = new QActionGroup(this);
                        group->setExclusionPolicy(sourceGroup->exclusionPolicy());
                    }
                    group->addAction(action);
                }
                actionCollection()->addAction(source->objectName(), action);
                KActionCollection::setShortcutsConfigurable(action, KActionCollection::isShortcutsConfigurable(source));
                connect(action, &QAction::triggered, this, [this, name = source->objectName()] {
                    const QPointer<WindowMenus> guard(this);
                    if (auto* target = selectAction(name); target && target->isEnabled())
                        target->trigger();
                    // The callback can unregister the last document and delete
                    // this controller before trigger() returns.
                    if (guard)
                        guard->refreshActions();
                });
            }
            for (QAction* source : actions) {
                if (!source->menu())
                    continue;
                auto* menu = actionCollection()->action(source->objectName())->menu();
                for (QAction* child : source->menu()->actions()) {
                    auto* action = actionCollection()->action(child->objectName());
                    if (!action) {
                        action = new QAction(child->text(), this);
                        action->setEnabled(child->isEnabled());
                        m_unavailable = action;
                    }
                    menu->addAction(action);
                }
                connect(menu, &QMenu::aboutToShow, this, [this, name = source->objectName()] {
                    const QPointer<WindowMenus> guard(this);
                    if (auto* source = selectAction(name))
                        QMetaObject::invokeMethod(source->menu(), "aboutToShow", Qt::DirectConnection);
                    if (guard)
                        guard->refreshActions();
                });
            }
            setXMLFile(QStringLiteral("mupdfngui.rc"));
        }
        for (QAction* source : actions) {
            connect(source, &QAction::changed, this, [this, owner] {
                if (isActiveOwner(owner))
                    refreshActions();
            });
        }
        refreshActions();
    }

    bool removeClient(KXMLGUIClient* client)
    {
        for (QAction* action : client->KXMLGUIClient::actionCollection()->actions())
            disconnect(action, nullptr, this, nullptr);
        m_clients.removeIf([client](const Client& entry) { return entry.actions == client; });
        if (m_clients.isEmpty())
            return true;
        refreshActions();
        return false;
    }

private:
    friend class MenuRegistration;

    bool isActiveOwner(const std::optional<QPointer<QObject>>& owner) const
    {
        if (!owner)
            return true;
        const auto* client = dynamic_cast<KXMLGUIClient*>(owner->data());
        return client && factory() && client->factory() == factory();
    }

    KActionCollection* selectActionCollection() const
    {
        for (const auto& client : m_clients) {
            // Okular removes the inactive part from the factory but leaves its
            // generator registered. Never borrow availability from that tab.
            if (isActiveOwner(client.owner))
                return client.actions->KXMLGUIClient::actionCollection();
        }
        return nullptr;
    }

    QAction* selectAction(const QString& name) const
    {
        auto* collection = selectActionCollection();
        return collection ? collection->action(name) : nullptr;
    }

    void refreshActions()
    {
        const auto* collection = selectActionCollection();
        for (QAction* action : actionCollection()->actions()) {
            if (auto* source = collection ? collection->action(action->objectName()) : nullptr) {
                action->setEnabled(source->isEnabled());
                action->setVisible(source->isVisible());
                action->setChecked(source->isChecked());
            } else
                action->setEnabled(false);
        }
        if (m_unavailable) {
            auto* mode = actionCollection()->action(QStringLiteral("mupdf_ocr_off"));
            m_unavailable->setVisible(mode && !mode->isVisible());
        }
    }

    struct Client {
        KXMLGUIClient* actions;
        // No owner denotes a standalone client; a null guarded owner denotes
        // a destroyed part and must never fall back to standalone routing.
        std::optional<QPointer<QObject>> owner;
    };

    QList<Client> m_clients;
    QAction* m_unavailable = nullptr;
    bool m_rebuildPending = false;
    std::optional<int> m_previousOcrMode;
};

namespace {

QHash<KXMLGUIFactory*, QPointer<WindowMenus>>& windowMenus()
{
    static QHash<KXMLGUIFactory*, QPointer<WindowMenus>> menus;
    return menus;
}

} // namespace

MenuRegistration::MenuRegistration(KXMLGUIClient* client)
    : m_client(client)
{
    for (QAction* action : m_client->KXMLGUIClient::actionCollection()->actions()) {
        if (KActionCollection::isShortcutsConfigurable(action))
            m_configurable.append(action);
    }
}

MenuRegistration::~MenuRegistration()
{
    detach();
}

int MenuRegistration::nextOcrMode(int currentMode)
{
    using Mode = MuPDFNGSettings::EnumOcrTriggerMode;
    // Headless clients retain their own history; registered tabs share the
    // window's history so closing the original tab does not lose it.
    auto& previousMode = m_menus ? m_menus->m_previousOcrMode : m_previousOcrMode;
    if (currentMode == Mode::Always)
        return previousMode.value_or(Mode::Never);
    previousMode = currentMode;
    return Mode::Always;
}

void MenuRegistration::schedule()
{
    const auto* collection = m_client->KXMLGUIClient::actionCollection();
    if (m_pending || collection->associatedWidgets().isEmpty())
        return;
    // Capture the part synchronously while XMLGUI registers its generator;
    // another tab may already be active when the deferred attach runs.
    if (!m_owner) {
        auto* factory = m_client->factory();
        // beginXMLPlug associates the window before setFactory is called.
        if (!factory) {
            for (auto* widget : collection->associatedWidgets()) {
                if (auto* window = qobject_cast<KXmlGuiWindow*>(widget)) {
                    factory = window->guiFactory();
                    break;
                }
            }
        }
        if (factory) {
            for (auto* candidate : factory->clients()) {
                auto* object = dynamic_cast<QObject*>(candidate);
                if (qobject_cast<Okular::ViewerInterface*>(object)) {
                    m_owner = object;
                    break;
                }
            }
        }
    }
    // XMLGUI exposes the factory only after building the client. Defer binding
    // until addClient has completed; the QObject context cancels pending work.
    m_pending = true;
    QTimer::singleShot(0, this, [this] {
        m_pending = false;
        attach();
    });
}

void MenuRegistration::attach()
{
    auto* factory = m_client->factory();
    if (!factory)
        return;
    if (m_factory != factory) {
        const auto owner = m_owner;
        detach();
        m_owner = owner;
        m_factory = factory;
        auto& menus = windowMenus()[factory];
        if (!menus) {
            menus = new WindowMenus(factory);
            menus->m_previousOcrMode = m_previousOcrMode;
            connect(factory, &QObject::destroyed, menus, [factory] { windowMenus().remove(factory); });
        }
        m_menus = menus;
        m_menus->addClient(m_client, m_owner);
        for (QAction* action : m_configurable)
            KActionCollection::setShortcutsConfigurable(action, false);
        m_removed = connect(factory, &KXMLGUIFactory::clientRemoved, this, [this](KXMLGUIClient* client) {
            if (client == m_client)
                detach();
        });
    }
    // Only the shared collection participates in shortcut dispatch/editing.
    // The per-document collections continue to hold document availability.
    m_client->KXMLGUIClient::actionCollection()->clearAssociatedWidgets();
    factory->addClient(m_menus);
}

void MenuRegistration::detach()
{
    disconnect(m_removed);
    for (QAction* action : m_configurable)
        KActionCollection::setShortcutsConfigurable(action, true);
    if (m_menus)
        m_previousOcrMode = m_menus->m_previousOcrMode;
    if (m_menus && m_menus->removeClient(m_client)) {
        windowMenus().remove(m_factory);
        if (m_menus->factory())
            m_menus->factory()->removeClient(m_menus);
        delete m_menus.data();
    }
    m_menus = nullptr;
    m_factory = nullptr;
    m_owner.reset();
}

} // namespace Mu::Generator
