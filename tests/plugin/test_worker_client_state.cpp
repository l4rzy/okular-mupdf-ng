// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "plugin/worker_client.hpp"

using Mu::Plugin::WorkerClient;
using State = Mu::Plugin::WorkerClient::State;

class TestWorkerClientState : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void initialStateIsStoppedAndNotOperational()
    {
        WorkerClient client;
        QCOMPARE(client.state(), State::Stopped);
        QVERIFY(!client.operational());

        // Stopping an unstarted client is idempotent and keeps it gated.
        client.stop();
        QCOMPARE(client.state(), State::Stopped);
        QVERIFY(!client.operational());
    }

    void committingASessionPublishesReady()
    {
        WorkerClient client;

        client.commitSessionReady();
        QCOMPARE(client.state(), State::Ready);
        QVERIFY(client.operational());

        // A redundant commit leaves the session ready.
        client.commitSessionReady();
        QCOMPARE(client.state(), State::Ready);
        QVERIFY(client.operational());
    }

    void failedRecoveryIsTerminalUntilRestart()
    {
        WorkerClient client;
        client.commitSessionReady();
        QVERIFY(client.operational());

        client.commitSessionFailed();
        QCOMPARE(client.state(), State::Failed);
        QVERIFY(!client.operational());

        // Only an explicit restart returns to Ready.
        client.commitSessionReady();
        QCOMPARE(client.state(), State::Ready);
        QVERIFY(client.operational());
    }
};

QTEST_GUILESS_MAIN(TestWorkerClientState)

#include "test_worker_client_state.moc"
