// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCoreApplication>
#include <QTest>

#include <climits>

#include "plugin/util/process_memory.hpp"

namespace ProcessMemory = Mu::Plugin::Util;

class TestPluginProcessMemory : public QObject {
    Q_OBJECT

private slots:

    void ownProcessReportsResidentMemory()
    {
        const auto resident = ProcessMemory::processResidentBytes(QCoreApplication::applicationPid());
        QVERIFY(resident.has_value());
        QVERIFY(*resident > 0);
    }

    void invalidPidReturnsNullopt()
    {
        QVERIFY(!ProcessMemory::processResidentBytes(0).has_value());
        QVERIFY(!ProcessMemory::processResidentBytes(-1).has_value());
    }

    void nonexistentPidReturnsNullopt() { QVERIFY(!ProcessMemory::processResidentBytes(INT_MAX).has_value()); }
};

QTEST_GUILESS_MAIN(TestPluginProcessMemory)

#include "test_process_memory.moc"
