// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_transport.hpp"
#include "shared/protocol/zpp_codec.hpp"

#include <QSignalSpy>
#include <QTest>

#include <chrono>
#include <sys/socket.h>

namespace Mu::Plugin {

class TestWorkerTransport : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void receiveResponse_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<bool>("success");
        QTest::newRow("matching-response") << 0 << true;
        QTest::newRow("inline-notification") << 1 << true;
        QTest::newRow("wrong-response-id") << 2 << false;
        QTest::newRow("malformed-frame") << 3 << false;
        QTest::newRow("closed-peer") << 4 << false;
        QTest::newRow("expired-deadline") << 5 << false;
        QTest::newRow("unauthorized-signing") << 6 << false;
        QTest::newRow("read-timeout") << 7 << false;
    }

    void receiveResponse()
    {
        QFETCH(int, scenario);
        QFETCH(bool, success);
        WorkerTransport transport;
        int sockets[2];
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        transport.m_ctrl = IPC::CtrlChannel(sockets[0]);
        IPC::CtrlChannel peer(sockets[1]);
        const QSignalSpy notifications(&transport, &WorkerTransport::ocrDone);
        transport.m_inFlight = true;
        transport.m_signingAuthorization = { 42, "selected" };
        transport.m_activeSignPassword = QStringLiteral("secret");
        std::string error;
        const auto write = [&](const auto& message) {
            return IPC::ZppCodec::writeMessage(peer, message, 1000, &error, "test");
        };
        if (scenario == 1) {
            QVERIFY(write(Model::NotificationMessage { Model::OcrDoneNotification { 7, 3 } }));
        }
        if (scenario <= 2) {
            QVERIFY(write(Model::ResponseMessage { scenario == 2 ? 43u : 42u, std::monostate { }, std::nullopt }));
        } else if (scenario == 3) {
            const std::byte invalid[] { std::byte { 0xff } };
            QVERIFY(IPC::writeFrame(peer, invalid, 1000, &error));
        } else if (scenario == 4) {
            peer.close();
        } else if (scenario == 6) {
            Model::SignInput sign;
            sign.jobId = 99;
            QVERIFY(write(Model::NotificationMessage { sign }));
        }
        const auto timeout = std::chrono::milliseconds(scenario == 5 ? -1 : scenario == 7 ? 50 : 1000);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto response = transport.receiveResponse(42, deadline);
        QCOMPARE(response.has_value(), success);
        QCOMPARE(transport.m_ctrl.valid(), success);
        QCOMPARE(notifications.size(), scenario == 1 ? 1 : 0);
        if (success) {
            QCOMPARE(response->id, 42u);
            if (scenario == 1) {
                QCOMPARE(notifications.at(0).at(0).toULongLong(), 7u);
                QCOMPARE(notifications.at(0).at(1).toInt(), 3);
            }
        } else {
            QVERIFY(!transport.m_inFlight);
            QCOMPARE(transport.m_signingAuthorization.requestId, 0u);
            QVERIFY(transport.m_activeSignPassword.isEmpty());
        }
    }

    void publishResponse_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<bool>("reportFailure");
        QTest::addColumn<int>("errors");
        QTest::addColumn<bool>("throwNotification");
        QTest::newRow("open-generation") << 0 << false << 0 << false;
        QTest::newRow("reported-error") << 1 << true << 1 << false;
        QTest::newRow("silent-error") << 1 << false << 0 << false;
        QTest::newRow("cancelled-error") << 2 << true << 0 << false;
        QTest::newRow("notification-exception") << 0 << false << 0 << true;
    }

    void publishResponse()
    {
        QFETCH(int, scenario);
        QFETCH(bool, reportFailure);
        QFETCH(int, errors);
        QFETCH(bool, throwNotification);
        WorkerTransport transport;
        QVERIFY(transport.start(QStringLiteral(WORKER_BUILD_PATH), { }, nullptr));
        // Retain the worker's actual channel while a scripted peer supplies the
        // exchange. The process stays alive for call()'s connection check.
        auto workerChannel = std::move(transport.m_ctrl);
        int sockets[2];
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        transport.m_ctrl = IPC::CtrlChannel(sockets[0]);
        IPC::CtrlChannel peer(sockets[1]);
        transport.m_notifier = std::make_unique<QSocketNotifier>(sockets[0], QSocketNotifier::Read);
        const QSignalSpy failures(&transport, &WorkerTransport::operationFailed);
        const QSignalSpy links(&transport, &WorkerTransport::pageLinksReady);
        Model::ResponseMessage expected { transport.m_nextId, Model::OpenResponse { { }, 123, { } }, std::nullopt };
        if (scenario != 0) {
            expected.payload = std::monostate { };
            expected.error = Model::Error {
                scenario == 2 ? Model::ErrorCode::Cancelled : Model::ErrorCode::InvalidRequest, "open", "test failure"
            };
        }
        std::string error;
        QVERIFY(IPC::ZppCodec::writeMessage(peer, expected, 1000, &error, "test"));
        // This frame must be drained only after the open response publishes its
        // generation. Otherwise handleNotification() discards it as stale.
        QVERIFY(IPC::ZppCodec::writeMessage(
            peer,
            Model::NotificationMessage { Model::PageLinksNotification { 123, { }, false, { } } },
            1000,
            &error,
            "test"));
        transport.m_activeSignPassword = QStringLiteral("secret");
        const auto epoch = transport.documentEpoch();

        struct NotificationFailure { };

        if (throwNotification) {
            connect(&transport, &WorkerTransport::pageLinksReady, &transport, [&] {
                QVERIFY(!transport.m_inFlight);
                QVERIFY(transport.m_activeSignPassword.isEmpty());
                QCOMPARE(transport.m_signingAuthorization.requestId, 0u);
                QVERIFY(transport.m_notifier->isEnabled());
                throw NotificationFailure { };
            });
            QVERIFY_THROWS_EXCEPTION(NotificationFailure, transport.call(Model::OpenRequest { }, reportFailure));
        } else {
            const auto response = transport.call(Model::OpenRequest { }, reportFailure);
            QVERIFY(response);
        }
        QCOMPARE(transport.documentEpoch(), epoch + 1);
        QCOMPARE(failures.size(), errors);
        QCOMPARE(links.size(), scenario == 0 ? 1 : 0);
        QCOMPARE(transport.m_linkGeneration, scenario == 0 ? 123u : 0u);
        QVERIFY(!transport.m_inFlight);
        QVERIFY(transport.m_activeSignPassword.isEmpty());
        QCOMPARE(transport.m_signingAuthorization.requestId, 0u);
        QVERIFY(transport.m_notifier->isEnabled());
    }
};

} // namespace Mu::Plugin

QTEST_GUILESS_MAIN(Mu::Plugin::TestWorkerTransport)

#include "test_worker_transport.moc"
