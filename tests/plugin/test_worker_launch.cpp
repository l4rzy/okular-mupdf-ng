// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/worker_launch.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <fcntl.h>

using namespace Mu::Plugin;

class TestWorkerLaunch : public QObject {
    Q_OBJECT
private slots:

    void parsesPid_data()
    {
        QTest::addColumn<QByteArray>("status");
        QTest::addColumn<qint64>("expected");
        QTest::newRow("valid") << QByteArray(R"({"child-pid":123,"other":true})") << qint64(123);
        QTest::newRow("partial") << QByteArray(R"({"child-pid":)") << qint64(-1);
        QTest::newRow("missing") << QByteArray("{}") << qint64(-1);
        QTest::newRow("string") << QByteArray(R"({"child-pid":"123"})") << qint64(-1);
        QTest::newRow("zero") << QByteArray(R"({"child-pid":0})") << qint64(-1);
        QTest::newRow("negative") << QByteArray(R"({"child-pid":-1})") << qint64(-1);
        QTest::newRow("fractional") << QByteArray(R"({"child-pid":1.5})") << qint64(-1);
        QTest::newRow("overflow") << QByteArray(R"({"child-pid":2147483648})") << qint64(-1);
    }

    void parsesPid()
    {
        QFETCH(QByteArray, status);
        QFETCH(qint64, expected);
        QCOMPARE(parseBubblewrapPid(status).value_or(-1), expected);
    }

    void preservesArguments()
    {
        const QString worker = QStringLiteral("/opt/test worker");
        const QStringList input { QStringLiteral("--socket"), QStringLiteral("/tmp/session/socket with spaces") };
        const auto arguments = buildBubblewrapArguments(worker,
                                                        input,
                                                        QStringLiteral("/tmp/session"),
                                                        { QStringLiteral("/usr"), QStringLiteral("/opt/ocr data") },
                                                        7);
        const auto separator = arguments.indexOf(QStringLiteral("--"));
        QCOMPARE(arguments.mid(separator + 1), QStringList({ worker }) + input);
        const auto bind = arguments.indexOf(QStringLiteral("--bind"));
        QCOMPARE(arguments.mid(bind, 3), QStringList({ "--bind", "/tmp/session", "/tmp/session" }));
        QVERIFY(arguments.contains(QStringLiteral("/opt/ocr data")));
        QVERIFY(arguments.contains(QStringLiteral("--unshare-net")));
        QVERIFY(arguments.contains(QStringLiteral("--clearenv")));
        QVERIFY(!arguments.contains(QStringLiteral("--unshare-pid")));
    }

    void isolatesFilesystem()
    {
        const auto launcher = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
        if (launcher.isEmpty())
            QSKIP("Bubblewrap is unavailable");
        QTemporaryDir session;
        QVERIFY(session.isValid());
        QTemporaryFile secret;
        QVERIFY(secret.open());
        QTemporaryDir readOnlyDirectory;
        QVERIFY(readOnlyDirectory.isValid());
        QFile readOnlyFile(readOnlyDirectory.filePath(QStringLiteral("data")));
        QVERIFY(readOnlyFile.open(QIODevice::WriteOnly));
        QCOMPARE(readOnlyFile.write("original"), 8);
        readOnlyFile.close();
        QTemporaryFile status;
        QVERIFY(status.open());
        const int statusFd = status.handle();
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("BWRAP_TEST_SECRET"), QStringLiteral("hidden"));
        process.setProcessEnvironment(environment);
        process.setChildProcessModifier([&] {
            if (::fcntl(statusFd, F_SETFD, 0) < 0)
                process.failChildProcessModifier("preserve status FD", errno);
        });
        QStringList paths;
        for (const auto* path : { "/usr", "/lib", "/lib64" }) {
            if (QFileInfo::exists(QString::fromLatin1(path)))
                paths.append(QString::fromLatin1(path));
        }
        paths.append(readOnlyDirectory.path());
        const auto shell = QFileInfo(QStandardPaths::findExecutable(QStringLiteral("sh"))).canonicalFilePath();
        QVERIFY(!shell.isEmpty());
        const auto script = QStringLiteral("test ! -e \"$1\" && test -z \"$BWRAP_TEST_SECRET\" && "
                                           "test ! -w /usr && test -w /tmp && test -w \"$2\" && "
                                           "test -r \"$3\" && ! (echo modified > \"$3\") 2>/dev/null && "
                                           "test ! -r \"/proc/$4/root$1\" && readlink /proc/self/ns/net");
        process.start(launcher,
                      buildBubblewrapArguments(shell,
                                               { "-c",
                                                 script,
                                                 "probe",
                                                 secret.fileName(),
                                                 session.path(),
                                                 readOnlyFile.fileName(),
                                                 QString::number(QCoreApplication::applicationPid()) },
                                               session.path(),
                                               paths,
                                               statusFd));
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        const auto error = process.readAllStandardError();
        if (error.startsWith("bwrap:")
            && (error.contains("Operation not permitted") || error.contains("Permission denied")
                || error.contains("No permissions to create a new namespace")))
            QSKIP(error.constData());
        QVERIFY2(process.exitCode() == 0, error.constData());
        QVERIFY(status.seek(0));
        QVERIFY(parseBubblewrapPid(status.readAll()));
        const auto network = process.readAllStandardOutput().trimmed();
        QVERIFY(!network.isEmpty());
        QVERIFY(network != QFileInfo(QStringLiteral("/proc/self/ns/net")).symLinkTarget().section('/', -1).toUtf8());
    }
};

QTEST_GUILESS_MAIN(TestWorkerLaunch)
#include "test_worker_launch.moc"
