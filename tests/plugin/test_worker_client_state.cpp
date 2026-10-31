// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "plugin/util/request_timeout.hpp"
#include "plugin/worker_client.hpp"
#include "shared/transport/common.hpp"

using Mu::Plugin::WorkerClient;
using State = Mu::Plugin::WorkerClient::State;

class TestWorkerClientState : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void bookRequestTimeouts_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("timeout");
        QTest::newRow("kindle-open") << 0 << Mu::IPC::Timeout::ExportMs;
        QTest::newRow("epub-open") << 1 << Mu::IPC::Timeout::GenericOpMs;
        QTest::newRow("epub-export") << 2 << Mu::IPC::Timeout::ExportMs;
        QTest::newRow("azw3-export") << 3 << Mu::IPC::Timeout::ExportMs;
        QTest::newRow("ordinary-save") << 4 << Mu::IPC::Timeout::GenericOpMs;
        QTest::newRow("pdf-print") << 5 << 30'000;
        QTest::newRow("pdf-flatten") << 6 << 30'000;
        QTest::newRow("pdf-with-references") << 7 << 30'000;
        QTest::newRow("pdf-background-submission") << 8 << Mu::IPC::Timeout::GenericOpMs;
    }

    void bookRequestTimeouts()
    {
        QFETCH(int, kind);
        QFETCH(int, timeout);
        Mu::Model::RequestPayload payload;
        if (kind < 2) {
            Mu::Model::OpenRequest open;
            open.documentType = kind == 0 ? Mu::Model::DocumentType::Kindle : Mu::Model::DocumentType::Epub;
            payload = open;
        } else if (kind < 5) {
            Mu::Model::SaveRequest save;
            save.exportEpub = kind == 2;
            save.exportAzw3 = kind == 3;
            payload = save;
        } else if (kind < 8) {
            Mu::Model::SavePdfRequest save;
            save.flatten = kind == 6;
            save.withReferences = kind == 7;
            payload = save;
        } else {
            payload = Mu::Model::ExportPdfAsyncRequest { };
        }
        QCOMPARE(Mu::Plugin::Util::requestTimeoutFor(payload), timeout);
    }

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

    void failedSessionGatesOperationsUntilReadyIsCommitted()
    {
        WorkerClient client;
        client.commitSessionReady();
        QVERIFY(client.operational());

        client.commitSessionFailed();
        QCOMPARE(client.state(), State::Failed);
        QVERIFY(!client.operational());

        // A newly committed session restores operational access.
        client.commitSessionReady();
        QCOMPARE(client.state(), State::Ready);
        QVERIFY(client.operational());
    }

    void renderWithoutWorkerCompletes_data()
    {
        QTest::addColumn<int>("abortAfter");
        QTest::newRow("completion") << 0;
        QTest::newRow("already-cancelled") << 1;
        QTest::newRow("cancel-after-enqueue") << 2;
    }

    void renderWithoutWorkerCompletes()
    {
        QFETCH(int, abortAfter);
        WorkerClient client;
        int checks = 0;
        QVERIFY(client.render(0, 100, 100, { }, [&] { return abortAfter && ++checks >= abortAfter; }).isNull());
        QCOMPARE(client.state(), State::Stopped);
        client.stop();
    }
};

QTEST_GUILESS_MAIN(TestWorkerClientState)

#include "test_worker_client_state.moc"
