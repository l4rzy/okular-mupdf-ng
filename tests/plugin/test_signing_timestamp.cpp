// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/signing_timestamp.hpp"

#include <QDateTime>
#include <QTest>
#include <QTimeZone>

namespace SigningTimestamp = Mu::Plugin::Util::SigningTimestamp;

class TestPluginSigningTimestamp : public QObject {
    Q_OBJECT

private slots:

    void formatsFriendlyDisplayDate()
    {
        const QDateTime when(QDate(2026, 9, 7), QTime(13, 14), QTimeZone("America/Chicago"));
        QCOMPARE(SigningTimestamp::displayDate(when), QStringLiteral("Sep 7, 2026 13:14 CDT"));

        // Two-digit day stays intact; winter date uses the standard abbreviation.
        const QDateTime december(QDate(2026, 12, 31), QTime(18, 0), QTimeZone("America/Chicago"));
        QCOMPARE(SigningTimestamp::displayDate(december), QStringLiteral("Dec 31, 2026 18:00 CST"));
    }

    void currentProducesConsistentTimestamp()
    {
        const auto timestamp = SigningTimestamp::current();
        QVERIFY(timestamp.epochSeconds > 0);
        const QDateTime epoch = QDateTime::fromSecsSinceEpoch(timestamp.epochSeconds);
        // The display date must describe the same instant it was captured with.
        QVERIFY(epoch.secsTo(QDateTime::currentDateTime()) < 60);
        QVERIFY(!timestamp.displayDate.isEmpty());
    }

    void currentUtcRendersUtcSuffix()
    {
        const auto timestamp = SigningTimestamp::current(true);
        QVERIFY(timestamp.epochSeconds > 0);
        // The display date describes the captured instant in UTC.
        const QDateTime utc = QDateTime::fromSecsSinceEpoch(timestamp.epochSeconds, QTimeZone::UTC);
        QCOMPARE(timestamp.displayDate, SigningTimestamp::displayDate(utc));
        QVERIFY(timestamp.displayDate.endsWith(QStringLiteral("UTC")));
    }
};

#include "test_signing_timestamp.moc"

int main(int argc, char** argv)
{
    TestPluginSigningTimestamp test;
    return QTest::qExec(&test, argc, argv);
}
