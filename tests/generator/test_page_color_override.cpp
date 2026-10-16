// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/page_color_override.hpp"

#include <KConfig>
#include <KConfigGroup>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <array>
#include <okular/core/settings_core.h>

using Mu::Generator::PageColorOverride;
using Mu::Model::ReadingTheme;
using Settings = Okular::SettingsCore;

namespace {

std::array<QString, 3> readSavedColors()
{
    KConfig disk(Settings::self()->config()->name(), KConfig::SimpleConfig);
    std::array<QString, 3> values;
    const std::array<QString, 3> names { "ChangeColors", "RenderMode", "PaperColor" };
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto* item = Settings::self()->findItem(names[i]);
        values[i] = KConfigGroup(&disk, item->group()).readEntry(item->key(), QStringLiteral("<absent>"));
    }
    return values;
}

} // namespace

class TestPageColorOverride : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.path().toUtf8());
        Settings::instance(QStringLiteral("page-color-override-test"));
    }

    void restoresPreferences_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<int>("mode");
        QTest::newRow("disabled") << false << int(Settings::EnumRenderMode::Inverted);
        QTest::newRow("inverted") << true << int(Settings::EnumRenderMode::Inverted);
        QTest::newRow("custom-paper") << true << int(Settings::EnumRenderMode::Paper);
    }

    void restoresPreferences()
    {
        QFETCH(bool, enabled);
        QFETCH(int, mode);
        const QColor paper(121, 140, 159);
        Settings::setChangeColors(enabled);
        Settings::setRenderMode(mode);
        Settings::setPaperColor(paper);
        QVERIFY(Settings::self()->save());
        const auto saved = readSavedColors();
        const std::array<QString, 3> names { "ChangeColors", "RenderMode", "PaperColor" };
        const auto readFlags = [](KConfigSkeletonItem* item) {
            if (auto* signalling = dynamic_cast<KConfigCompilerSignallingItem*>(item))
                return signalling->writeFlags();
            return item->writeFlags();
        };
        std::array<KConfigBase::WriteConfigFlags, 3> flags;
        for (std::size_t i = 0; i < names.size(); ++i)
            flags[i] = readFlags(Settings::self()->findItem(names[i]));
        {
            PageColorOverride override;
            override.setTheme(ReadingTheme::Dusk);
            QCOMPARE(Settings::changeColors(), enabled);
            QCOMPARE(Settings::renderMode(), mode);
            QCOMPARE(Settings::paperColor(), paper);
            override.setTheme(ReadingTheme::Dark);
            for (const auto& name : names)
                QVERIFY(!readFlags(Settings::self()->findItem(name)).testFlag(KConfigBase::Persistent));
            QVERIFY(Settings::changeColors());
            QCOMPARE(Settings::renderMode(), int(Settings::EnumRenderMode::Paper));
            QCOMPARE(Settings::paperColor(), QColor(32, 33, 36));
            QCOMPARE(PageColorOverride::originalPaperColor(),
                     enabled && mode == Settings::EnumRenderMode::Paper ? paper : QColor(Qt::white));
            // A host save must preserve disk preferences even while another
            // persistent setting forces the configuration file to be written.
            Settings::setEnableThreading(!Settings::enableThreading());
            QVERIFY(Settings::self()->save());
            QCOMPARE(readSavedColors(), saved);
            override.setTheme(ReadingTheme::Default);
            QCOMPARE(Settings::changeColors(), enabled);
            QCOMPARE(Settings::renderMode(), mode);
            QCOMPARE(Settings::paperColor(), paper);
            override.setTheme(ReadingTheme::Dark);
            override.setTheme(ReadingTheme::Dusk);
            QCOMPARE(Settings::changeColors(), enabled);
            QCOMPARE(Settings::renderMode(), mode);
            QCOMPARE(Settings::paperColor(), paper);
            override.setTheme(ReadingTheme::Dark);
        }
        QCOMPARE(Settings::changeColors(), enabled);
        QCOMPARE(Settings::renderMode(), mode);
        QCOMPARE(Settings::paperColor(), paper);
        for (std::size_t i = 0; i < names.size(); ++i)
            QCOMPARE(readFlags(Settings::self()->findItem(names[i])), flags[i]);
        QVERIFY(Settings::self()->save());
        QCOMPARE(readSavedColors(), saved);
    }

    void sharesOwnership()
    {
        Settings::setChangeColors(false);
        Settings::setRenderMode(Settings::EnumRenderMode::Inverted);
        Settings::setPaperColor(Qt::yellow);
        PageColorOverride first;
        PageColorOverride second;
        first.setTheme(ReadingTheme::Dark);
        first.setTheme(ReadingTheme::Dark);
        second.setTheme(ReadingTheme::Dark);
        first.reset();
        first.reset();
        QVERIFY(Settings::changeColors());
        QCOMPARE(Settings::paperColor(), QColor(32, 33, 36));
        second.reset();
        QVERIFY(!Settings::changeColors());
        QCOMPARE(Settings::renderMode(), int(Settings::EnumRenderMode::Inverted));
        QCOMPARE(Settings::paperColor(), QColor(Qt::yellow));
    }

    void publishesCompleteOverride()
    {
        Settings::setChangeColors(false);
        Settings::setRenderMode(Settings::EnumRenderMode::Inverted);
        Settings::setPaperColor(Qt::yellow);
        QVERIFY(Settings::self()->save());
        const auto saved = readSavedColors();
        std::array<QVariant, 3> expected { true, int(Settings::EnumRenderMode::Paper), QColor(32, 33, 36) };
        bool notified = false;
        const auto connection = connect(Settings::self(), &KCoreConfigSkeleton::configChanged, this, [&] {
            if (notified)
                return;
            notified = true;
            const std::array<QVariant, 3> values { Settings::changeColors(),
                                                   Settings::renderMode(),
                                                   Settings::paperColor() };
            QCOMPARE(values, expected);
            // Host callbacks can save from inside a settings notification.
            Settings::self()->save();
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        PageColorOverride override;
        override.setTheme(ReadingTheme::Dark);
        QVERIFY(!notified);
        QTRY_VERIFY(notified);
        QCOMPARE(readSavedColors(), saved);
        expected = { false, int(Settings::EnumRenderMode::Inverted), QColor(Qt::yellow) };
        notified = false;
        override.reset();
        QVERIFY(!notified);
        QTRY_VERIFY(notified);
        QCOMPARE(readSavedColors(), saved);
    }

    void respectsImmutableSettings_data()
    {
        QTest::addColumn<QByteArray>("entry");
        QTest::newRow("enabled") << QByteArray("ChangeColors");
        QTest::newRow("mode") << QByteArray("RenderMode");
        QTest::newRow("paper") << QByteArray("PaperColor");
    }

    void respectsImmutableSettings()
    {
        QFETCH(QByteArray, entry);
        const QString name = Settings::self()->config()->name();
        QFile file(QFileInfo(name).isAbsolute() ? name : m_root.filePath(name));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll();
        file.close();
        const auto restore = qScopeGuard([&] {
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                file.write(original);
                file.close();
            }
            Settings::self()->load();
        });
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[Document]\n" + entry + "[$i]="
                   + (entry == "ChangeColors"     ? "false"
                          : entry == "RenderMode" ? "Inverted"
                                                  : "121,140,159")
                   + '\n');
        file.close();
        Settings::self()->load();
        QVERIFY(Settings::self()->isImmutable(QString::fromLatin1(entry)));
        const bool enabled = Settings::changeColors();
        const int mode = Settings::renderMode();
        const QColor paper = Settings::paperColor();
        PageColorOverride override;
        override.setTheme(ReadingTheme::Dark);
        QCOMPARE(Settings::changeColors(), enabled);
        QCOMPARE(Settings::renderMode(), mode);
        QCOMPARE(Settings::paperColor(), paper);
    }
};

QTEST_GUILESS_MAIN(TestPageColorOverride)
#include "test_page_color_override.moc"
