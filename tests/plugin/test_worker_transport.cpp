// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_transport.hpp"
#include "shared/protocol/zpp_codec.hpp"
#include "support/export_progress.hpp"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <chrono>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace Mu::Plugin {

using Mu::Test::exportCompletions;

class TestWorkerTransport : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void exportProgress_data()
    {
        QTest::addColumn<quint64>("jobId");
        QTest::addColumn<int>("completed");
        QTest::addColumn<int>("total");
        QTest::addColumn<bool>("finalizing");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("pages") << quint64(42) << 2 << 10 << false << true;
        QTest::newRow("finalizing") << quint64(42) << 10 << 10 << true << true;
        QTest::newRow("stale") << quint64(41) << 2 << 10 << false << false;
        QTest::newRow("negative") << quint64(42) << -1 << 10 << false << false;
        QTest::newRow("empty") << quint64(42) << 0 << 0 << true << false;
        QTest::newRow("over-total") << quint64(42) << 11 << 10 << false << false;
        QTest::newRow("premature-finalizing") << quint64(42) << 2 << 10 << true << false;
        QTest::newRow("missing-finalizing") << quint64(42) << 10 << 10 << false << false;
    }

    void exportProgress()
    {
        QFETCH(quint64, jobId);
        QFETCH(int, completed);
        QFETCH(int, total);
        QFETCH(bool, finalizing);
        QFETCH(bool, accepted);
        WorkerTransport transport;
        transport.m_export = WorkerTransport::PendingExport { nullptr, { }, 42 };
        QSignalSpy progress(&transport, &WorkerTransport::exportProgress);
        const Model::NotificationMessage notification { Model::ExportProgressNotification {
            jobId,
            completed,
            total,
            finalizing ? Model::ExportPhase::Finalizing : Model::ExportPhase::CreatingPages,
            { } } };
        // Exercise the wire representation as well as transport validation.
        const auto bytes = IPC::ZppCodec::encode(notification);
        QVERIFY(bytes);
        Model::NotificationMessage decoded;
        QVERIFY(IPC::ZppCodec::decode(*bytes, &decoded));
        std::string error;
        QVERIFY(transport.handleNotification(decoded, &error));
        QCOMPARE(progress.size(), accepted ? 1 : 0);
        if (accepted) {
            const auto update = progress.front().front().value<Model::ExportProgressNotification>();
            QCOMPARE(update.jobId, jobId);
            QCOMPARE(update.completed, completed);
            QCOMPARE(update.total, total);
            QCOMPARE(update.phase == Model::ExportPhase::Finalizing, finalizing);
        }
        transport.completeExport(false, QStringLiteral("test failure"));
        QCOMPARE(exportCompletions(progress).size(), 1);
        QCOMPARE(exportCompletions(progress).front().phase, Model::ExportPhase::Failed);
        QCOMPARE(exportCompletions(progress).front().error, std::string("test failure"));
        QVERIFY(transport.handleNotification(notification, &error));
        QCOMPARE(progress.size(), accepted ? 2 : 1);
    }

    void exportCompletion_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::newRow("completion-during-submission") << 0;
        QTest::newRow("later-completion") << 1;
        QTest::newRow("submission-rejected") << 2;
        QTest::newRow("early-conversion-failure") << 3;
        QTest::newRow("publication-failure") << 4;
        QTest::newRow("late-after-timeout") << 5;
        QTest::newRow("stale-job-id") << 6;
        QTest::newRow("late-after-close") << 7;
        QTest::newRow("late-after-recovery") << 8;
    }

    void exportCompletion()
    {
        QFETCH(int, scenario);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString target = directory.filePath("export.pdf");
        if (scenario == 4)
            QVERIFY(QDir().mkdir(target));
        QFile destination(scenario == 4 ? target + QStringLiteral("/keep") : target);
        QVERIFY(destination.open(QIODevice::WriteOnly));
        QCOMPARE(destination.write("original"), 8);
        destination.close();
        QTemporaryFile source(directory.filePath("source-XXXXXX"));
        QVERIFY(source.open());
        WorkerTransport transport;
        QVERIFY(transport.start(QStringLiteral(WORKER_BUILD_PATH), { }, nullptr));
        transport.m_sourceFile = std::make_unique<QFile>(source.fileName());
        QVERIFY(transport.m_sourceFile->open(QIODevice::ReadOnly));
        auto workerChannel = std::move(transport.m_ctrl);
        int sockets[2];
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        transport.m_ctrl = IPC::CtrlChannel(sockets[0]);
        IPC::CtrlChannel peer(sockets[1]);
        transport.m_notifier = std::make_unique<QSocketNotifier>(sockets[0], QSocketNotifier::Read);
        transport.m_fd.close();
        IPC::FdChannel descriptors;
        const auto socketPath = QFile::encodeName(directory.filePath("fd.sock")).toStdString();
        std::string error;
        QVERIFY(descriptors.listen(socketPath, &error));
        QVERIFY(transport.m_fd.connect(socketPath, &error));
        QVERIFY(descriptors.accept(&error));
        const auto transfer = transport.m_nextTransfer;
        const QSignalSpy updates(&transport, &WorkerTransport::exportProgress);
        const Model::ResponseMessage discovery { transport.m_nextId,
                                                 Model::OutputFormatsResponse {
                                                     { { "pdf",
                                                         "application/pdf",
                                                         { "pdf" },
                                                         { { .id = ::Mu::Model::ExportProfileId::SourceWithReferences,
                                                             .synchronous = false,
                                                             .asynchronous = true } } } } },
                                                 std::nullopt };
        QVERIFY(IPC::ZppCodec::writeMessage(peer, discovery, 1000, &error));
        Model::ResponseMessage response { transport.m_nextId + 1, Model::JobResponse { 42 }, std::nullopt };
        if (scenario == 2) {
            response.payload = std::monostate { };
            response.error = Model::Error { Model::ErrorCode::InvalidRequest, "export", "rejected" };
        }
        const Model::NotificationMessage done { Model::ExportProgressNotification {
            42, 0, 0, scenario != 3 ? Model::ExportPhase::Succeeded : Model::ExportPhase::Failed, { } } };
        // Deliver the submission response and its notifications in one write,
        // after discovery has finished, to exercise completion already buffered
        // when startExport registers the returned job.
        std::vector<std::byte> frames;
        const auto appendMessage = [&](const auto& message) {
            const auto bytes = IPC::ZppCodec::encode(message);
            QVERIFY(bytes);
            const auto length = static_cast<std::uint32_t>(bytes->size());
            for (int shift : { 24, 16, 8, 0 })
                frames.push_back(static_cast<std::byte>((length >> shift) & 0xff));
            frames.insert(frames.end(), bytes->begin(), bytes->end());
        };
        appendMessage(response);
        if (scenario == 0) {
            appendMessage(Model::NotificationMessage {
                Model::ExportProgressNotification { 42, 2, 10, Model::ExportPhase::CreatingPages, { } } });
            appendMessage(Model::NotificationMessage {
                Model::ExportProgressNotification { 42, 10, 10, Model::ExportPhase::Finalizing, { } } });
        }
        if (scenario == 0 || scenario == 3 || scenario == 4)
            appendMessage(done);
        bool replied = false;
        std::jthread responder([&] {
            Model::RequestMessage discoveryRequest, exportRequest;
            if (!IPC::ZppCodec::readMessage(peer, &discoveryRequest, 1000)
                || !IPC::ZppCodec::readMessage(peer, &exportRequest, 1000))
                return;
            if (!std::holds_alternative<Model::OutputFormatsRequest>(discoveryRequest.payload)
                || !std::holds_alternative<Model::ExportRequest>(exportRequest.payload))
                return;
            replied =
                ::send(peer.fd(), frames.data(), frames.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(frames.size());
        });
        const auto job =
            transport.startExport(target, { "pdf", ::Mu::Model::ExportProfileId::SourceWithReferences, { } });
        responder.join();
        QVERIFY(replied);
        QCOMPARE(updates.size(), scenario == 0 ? 3 : (scenario == 3 || scenario == 4 ? 1 : 0));
        QCOMPARE(job.has_value(), scenario != 2);
        if (job)
            QCOMPARE(*job, 42u);
        const int outputFd = descriptors.receive(transfer, &error, 1000);
        QVERIFY(outputFd >= 0);
        ::close(outputFd);
        const int inputFd = descriptors.receive(transfer + 1, &error, 1000);
        QVERIFY(inputFd >= 0);
        ::close(inputFd);
        if (scenario == 1 || scenario >= 5) {
            QVERIFY(transport.m_export);
            QVERIFY(transport.m_exportTimer->isActive());
            QCOMPARE(exportCompletions(updates).size(), 0);
            if (scenario == 5) {
                // Exercise timeout cleanup directly, without waiting for a timer.
                QVERIFY(QMetaObject::invokeMethod(transport.m_exportTimer.get(), "timeout", Qt::DirectConnection));
            } else if (scenario == 8) {
                transport.abort();
            } else if (scenario == 7) {
                QVERIFY(IPC::ZppCodec::writeMessage(
                    peer,
                    Model::ResponseMessage { transport.m_nextId, std::monostate { }, std::nullopt },
                    1000,
                    &error));
                QVERIFY(transport.close());
            } else if (scenario == 6) {
                QVERIFY(transport.handleNotification(Model::NotificationMessage { Model::ExportProgressNotification {
                                                         41, 0, 0, Model::ExportPhase::Succeeded, { } } },
                                                     &error));
                QCOMPARE(exportCompletions(updates).size(), 0);
                QVERIFY(transport.m_export);
            }
            QVERIFY(transport.handleNotification(done, &error));
        }
        const bool successful = scenario == 0 || scenario == 1 || scenario == 6;
        const int expectedSignals = scenario == 2 || scenario == 7 ? 0 : 1;
        QCOMPARE(exportCompletions(updates).size(), expectedSignals);
        if (expectedSignals) {
            QCOMPARE(exportCompletions(updates).front().jobId, 42u);
            QCOMPARE(exportCompletions(updates).front().phase == Model::ExportPhase::Succeeded, successful);
            QCOMPARE(exportCompletions(updates).front().error.empty(), successful);
        }
        QVERIFY(!transport.m_export);
        QVERIFY(!transport.m_exportTimer || !transport.m_exportTimer->isActive());
        QVERIFY(destination.open(QIODevice::ReadOnly));
        // No engine runs in this protocol test: a successful staged file is empty.
        QCOMPARE(destination.readAll(), successful ? QByteArray() : QByteArray("original"));
        QVERIFY(QDir(directory.path())
                    .entryList({ QStringLiteral(".mupdfng-worker-*") }, QDir::Files | QDir::Hidden)
                    .isEmpty());
        QVERIFY(transport.handleNotification(done, &error));
        QCOMPARE(exportCompletions(updates).size(), expectedSignals);
    }

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
