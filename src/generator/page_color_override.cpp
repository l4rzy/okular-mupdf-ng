// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/page_color_override.hpp"

#include <KCoreConfigSkeleton>
#include <QColor>
#include <QPointer>
#include <QSignalBlocker>
#include <array>
#include <cstddef>
#include <okular/core/settings_core.h>

namespace Mu::Generator {
namespace {

struct SavedSetting {
    KConfigSkeletonItem* item = nullptr;
    QVariant value;
    KConfigBase::WriteConfigFlags flags;
};

struct SharedOverride {
    QPointer<KCoreConfigSkeleton> settings;
    std::size_t owners = 0;
    std::array<SavedSetting, 3> saved;
};

SharedOverride& sharedOverride()
{
    static SharedOverride state;
    return state;
}

// KF6's signalling wrapper shadows these non-virtual accessors. Use the
// wrapper accessors so flags reach the item that actually writes the value.
KConfigBase::WriteConfigFlags readWriteFlags(KConfigSkeletonItem* item)
{
    if (auto* signalling = dynamic_cast<KConfigCompilerSignallingItem*>(item))
        return signalling->writeFlags();
    return item->writeFlags();
}

void setWriteFlags(KConfigSkeletonItem* item, KConfigBase::WriteConfigFlags flags)
{
    if (auto* signalling = dynamic_cast<KConfigCompilerSignallingItem*>(item))
        signalling->setWriteFlags(flags);
    else
        item->setWriteFlags(flags);
}

void notifySettingsChanged(KCoreConfigSkeleton* settings)
{
    // Okular reparses generator settings synchronously on this signal.
    // Queue it so changing themes cannot re-enter a configuration update.
    QMetaObject::invokeMethod(
        settings,
        [settings] {
            // Keep Okular's color menu in sync after all three values are applied.
            if (settings->metaObject()->indexOfSignal("colorModesChanged()") >= 0)
                QMetaObject::invokeMethod(settings, "colorModesChanged", Qt::DirectConnection);
            Q_EMIT settings->configChanged();
        },
        Qt::QueuedConnection);
}

} // namespace

PageColorOverride::~PageColorOverride()
{
    reset();
}

QColor PageColorOverride::originalPaperColor()
{
    const auto& state = sharedOverride();
    if (state.owners && state.settings) {
        if (state.saved[0].value.toBool()
            && state.saved[1].value.toInt() == Okular::SettingsCore::EnumRenderMode::Paper)
            return state.saved[2].value.value<QColor>();
        return Qt::white;
    }
    return Okular::SettingsCore::changeColors()
            && Okular::SettingsCore::renderMode() == Okular::SettingsCore::EnumRenderMode::Paper
        ? Okular::SettingsCore::paperColor()
        : QColor(Qt::white);
}

void PageColorOverride::setTheme(Model::ReadingTheme theme)
{
    if (theme != Model::ReadingTheme::Dark) {
        reset();
        return;
    }

    auto& state = sharedOverride();
    auto* settings = Okular::SettingsCore::self();
    const std::array<KConfigSkeletonItem*, 3> items {
        settings->findItem(QStringLiteral("ChangeColors")),
        settings->findItem(QStringLiteral("RenderMode")),
        settings->findItem(QStringLiteral("PaperColor")),
    };
    for (auto* item : items)
        if (!item || item->isImmutable())
            return;

    if (!state.owners) {
        state.settings = settings;
        for (std::size_t i = 0; i < items.size(); ++i)
            state.saved[i] = { items[i], items[i]->property(), readWriteFlags(items[i]) };
    }
    if (!m_active) {
        ++state.owners;
        m_active = true;
    }

    // Match the worker's Dark viewing palette.
    const std::array<QVariant, 3> values {
        true,
        int(Okular::SettingsCore::EnumRenderMode::Paper),
        QColor(32, 33, 36),
    };
    bool changed = false;
    // Okular's color-menu signals can save settings synchronously. Publish
    // only a complete override, after every setting has non-persistent flags.
    const QSignalBlocker blocker(settings);
    for (std::size_t i = 0; i < items.size(); ++i) {
        // Notify includes Persistent; clearing both prevents a later host save
        // from persisting the temporary values or broadcasting them over D-Bus.
        setWriteFlags(items[i], state.saved[i].flags & ~KConfigBase::WriteConfigFlags(KConfigBase::Notify));
        changed |= items[i]->property() != values[i];
        items[i]->setProperty(values[i]);
    }
    if (changed)
        notifySettingsChanged(settings);
}

void PageColorOverride::reset()
{
    if (!m_active)
        return;
    m_active = false;
    auto& state = sharedOverride();
    if (--state.owners)
        return;
    if (state.settings) {
        const QSignalBlocker blocker(state.settings);
        for (const auto& saved : state.saved) {
            saved.item->setProperty(saved.value);
            setWriteFlags(saved.item, saved.flags);
        }
        notifySettingsChanged(state.settings);
    }
    state.settings.clear();
    state.saved = { };
}

} // namespace Mu::Generator
