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
        QCOMPARE(client.paperboxClient().state(), State::Stopped);
        QVERIFY(!client.operational());

        // Stopping an unstarted client is idempotent and keeps it gated.
        client.stop();
        QCOMPARE(client.paperboxClient().state(), State::Stopped);
        QVERIFY(!client.operational());
    }

    void committingASessionPublishesReady()
    {
        WorkerClient client;

        client.commitSessionReady();
        QCOMPARE(client.paperboxClient().state(), State::Ready);
        QVERIFY(client.operational());

        // A redundant commit leaves the session ready.
        client.commitSessionReady();
        QCOMPARE(client.paperboxClient().state(), State::Ready);
        QVERIFY(client.operational());
    }

    void failedSessionGatesOperationsUntilReadyIsCommitted()
    {
        WorkerClient client;
        client.commitSessionReady();
        QVERIFY(client.operational());

        client.commitSessionFailed();
        QCOMPARE(client.paperboxClient().state(), State::Failed);
        QVERIFY(!client.operational());

        // A newly committed session restores operational access.
        client.commitSessionReady();
        QCOMPARE(client.paperboxClient().state(), State::Ready);
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
        QCOMPARE(client.paperboxClient().state(), State::Stopped);
        client.stop();
    }

    void retainedFrameAndCancellation_data()
    {
        QTest::addColumn<QRect>("tile");
        QTest::newRow("full-page") << QRect();
        QTest::newRow("tile") << QRect(12, 16, 48, 32);
    }

    void retainedFrameAndCancellation()
    {
        QFETCH(QRect, tile);
        WorkerClient client;
        QVERIFY(client.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<WorkerClient::PageInfo> pages;
        QCOMPARE(client.open(QStringLiteral(TEST_PDF_PATH), { }, pages), Mu::Model::OpenStatus::Success);

        int checks = 0;
        QVERIFY(client.render(0, 128, 128, tile, [&] { return ++checks > 1; }).isNull());
        QVERIFY(checks >= 2);
        Mu::Model::RenderLinks links;
        const QImage frame = client.render(0, 128, 128, tile, { }, &links);
        QVERIFY(!frame.isNull());
        QCOMPARE(frame.size(), tile.isNull() ? QSize(128, 128) : tile.size());

        const QImage expected = frame.copy();
        client.stop();
        QCOMPARE(frame, expected);
    }
};

QTEST_GUILESS_MAIN(TestWorkerClientState)

#include "test_worker_client_state.moc"
