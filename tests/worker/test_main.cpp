// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <mupdf/fitz/version.h>

#include "shared/compat.hpp"
#include "shared/transport/fd_channel.hpp"

namespace {

struct WorkerResult {
    bool completed = false;
    int exitCode = -1;
    QByteArray output;
    QByteArray error;
    QString processError;
};

WorkerResult runWorker(const QStringList& arguments, const QString& directory = { })
{
    QProcess process;
    if (!directory.isEmpty())
        process.setWorkingDirectory(directory);
    process.start(QStringLiteral(WORKER_BUILD_PATH), arguments);
    WorkerResult result;
    if (process.waitForStarted(15000) && process.waitForFinished(15000)) {
        result.completed = process.exitStatus() == QProcess::NormalExit;
        result.exitCode = process.exitCode();
    } else {
        result.processError = process.errorString();
        process.kill();
        process.waitForFinished(15000);
    }
    result.output = process.readAllStandardOutput();
    result.error = process.readAllStandardError();
    if (!result.completed && result.processError.isEmpty())
        result.processError = QString::fromUtf8(result.error);
    return result;
}

} // namespace

class TestWorkerMain : public QObject {
    Q_OBJECT

private slots:

    void printsHelp_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("long") << QStringList { "--help" };
        QTest::newRow("short") << QStringList { "-h" };
        QTest::newRow("help-before-version") << QStringList { "--version", "--help" };
    }

    void printsHelp()
    {
        QFETCH(QStringList, arguments);
        const auto result = runWorker(arguments);
        QVERIFY2(result.completed, qPrintable(result.processError));
        QCOMPARE(result.exitCode, 0);
        QVERIFY(result.error.isEmpty());
        QVERIFY(result.output.contains("mupdfng-worker"));
        QVERIFY(result.output.contains("--socket"));
        QVERIFY(result.output.contains("--fd-socket"));
        QVERIFY(result.output.contains("--sandbox-check"));
        QVERIFY(result.output.contains("no IPC sockets are required"));
    }

    void printsVersion()
    {
        const auto result = runWorker({ "--version" });
        QVERIFY2(result.completed, qPrintable(result.processError));
        QCOMPARE(result.exitCode, 0);
        QVERIFY(result.error.isEmpty());
        const QByteArray expected = "mupdfng-worker "
            + QByteArray(::Mu::IPC::COMPAT.data(), static_cast<qsizetype>(::Mu::IPC::COMPAT.size())) + "\nMuPDF "
            + FZ_VERSION + "\n";
        QCOMPARE(result.output, expected);
    }

    void rejectsInvalidArguments_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QByteArray>("diagnostic");
        QTest::newRow("no-endpoints") << QStringList { } << QByteArray("socket");
        QTest::newRow("missing-control-endpoint") << QStringList { "--fd-socket", "unused" } << QByteArray("socket");
        QTest::newRow("missing-fd-endpoint") << QStringList { "--socket", "unused" } << QByteArray("fd-socket");
        QTest::newRow("missing-option-value") << QStringList { "--socket" } << QByteArray("socket");
        QTest::newRow("unknown-option") << QStringList { "--unknown-option" } << QByteArray("unknown-option");
        QTest::newRow("invalid-report-format")
            << QStringList { "--sandbox-check=xml" } << QByteArray("expected no format or '=json'");
        QTest::newRow("empty-report-format")
            << QStringList { "--sandbox-check=" } << QByteArray("expected no format or '=json'");
        QTest::newRow("repeated-report") << QStringList { "--sandbox-check", "--sandbox-check=json" }
                                         << QByteArray("only once");
        QTest::newRow("unexpected-positional")
            << QStringList { "--sandbox-check=json", "document.pdf" } << QByteArray("no positional arguments");
    }

    void rejectsInvalidArguments()
    {
        QFETCH(QStringList, arguments);
        QFETCH(QByteArray, diagnostic);
        const auto result = runWorker(arguments);
        QVERIFY2(result.completed, qPrintable(result.processError));
        QCOMPARE(result.exitCode, 2);
        QVERIFY(result.output.isEmpty());
        QVERIFY2(result.error.startsWith("error: "), result.error.constData());
        QVERIFY2(result.error.contains(diagnostic), result.error.constData());
        QVERIFY(result.error.contains("--sandbox-check"));
    }

    void reportsSandboxStatus_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<bool>("json");
        QTest::addColumn<bool>("customDirectories");
        QTest::newRow("implicit-human") << QStringList { "--sandbox-check" } << false << false;
        QTest::newRow("explicit-human") << QStringList { "--sandbox-check=human" } << false << false;
        QTest::newRow("json") << QStringList { "--sandbox-check=json" } << true << false;
        QTest::newRow("normalized-optional-directories") << QStringList { "--sandbox-check=json" } << true << true;
    }

    void reportsSandboxStatus()
    {
        QFETCH(QStringList, arguments);
        QFETCH(bool, json);
        QFETCH(bool, customDirectories);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        if (customDirectories) {
            QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("data")));
            // Repeated normalized paths and absent optional data are accepted.
            arguments << "--tessdata-dir=./data/../data" << "--tessdata-dir=data" << "--tessdata-dir="
                      << "--tessdata-dir=missing";
        }
        const auto result = runWorker(arguments, directory.path());
        QVERIFY2(result.completed, qPrintable(result.processError));
        if (json) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(result.output, &error);
            QCOMPARE(error.error, QJsonParseError::NoError);
            QVERIFY(document.isObject());
            const auto object = document.object();
            QCOMPARE(object.value("platform").toString(), QStringLiteral("linux"));
            QCOMPARE(object.value("profile").toString(), QStringLiteral("mupdfng.linux.v1"));
            QVERIFY(object.value("protections").isArray());
            QVERIFY(object.value("diagnostics").isArray());
            bool full = true;
            bool any = false;
            bool hasRequired = false;
            bool hasUnknown = false;
            QStringList remainingIds { "linux.no_new_privileges", "linux.namespaces", "linux.resource_limits",
                                       "linux.landlock",          "linux.seccomp",    "linux.mdwe" };
            for (const auto& entry : object.value("protections").toArray()) {
                const auto protection = entry.toObject();
                const auto id = protection.value("id").toString();
                QCOMPARE(remainingIds.removeAll(id), 1);
                QVERIFY(protection.value("required").isBool());
                QCOMPARE(protection.value("required").toBool(), id != "linux.mdwe");
                QVERIFY(protection.value("version").isString());
                QVERIFY(protection.value("diagnostic").isString());
                QVERIFY(QStringList({ "filesystem", "syscall", "process", "resources", "memory" })
                            .contains(protection.value("category").toString()));
                const auto state = protection.value("state").toString();
                QVERIFY(QStringList({ "unknown", "enforced", "unavailable", "disabled", "failed" }).contains(state));
                if (protection.value("required").toBool()) {
                    hasRequired = true;
                    full = full && state == "enforced";
                    any = any || state == "enforced";
                    hasUnknown = hasUnknown || state == "unknown";
                }
            }
            QVERIFY(remainingIds.isEmpty());
            full = full && hasRequired;
            QVERIFY(object.value("policySatisfied").isBool());
            QCOMPARE(object.value("policySatisfied").toBool(), full);
            QCOMPARE(object.value("isolation").toString(),
                     full             ? QStringLiteral("full")
                         : any        ? QStringLiteral("degraded")
                         : hasUnknown ? QStringLiteral("unknown")
                                      : QStringLiteral("none"));
            QCOMPARE(result.exitCode, full ? 0 : any ? 1 : 2);
        } else {
            const auto lines = result.output.split('\n');
            QVERIFY(lines.front().startsWith("Sandbox: linux profile=mupdfng.linux.v1 isolation="));
            QVERIFY(result.output.contains("linux.landlock"));
            QVERIFY(result.output.contains("linux.resource_limits"));
            const auto isolation = lines.front().split('=').back();
            QCOMPARE(result.exitCode, isolation == "full" ? 0 : isolation == "degraded" ? 1 : 2);
        }
    }

    void rejectsUnavailableEndpoints_data()
    {
        QTest::addColumn<bool>("fdListener");
        QTest::newRow("missing-fd-socket") << false;
        QTest::newRow("invalid-control-socket") << true;
    }

    void rejectsUnavailableEndpoints()
    {
        QFETCH(bool, fdListener);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString fdPath = directory.filePath(QStringLiteral("fd.sock"));
        ::Mu::IPC::FdChannel listener;
        if (fdListener) {
            std::string error;
            QVERIFY2(listener.listen(fdPath.toStdString(), &error), error.c_str());
        }
        // The descriptor listener is ready before launch. An overlong control
        // path fails synchronously, without an accept thread or connection race.
        const QString controlPath = directory.filePath(QString(256, QLatin1Char('x')));
        const auto result = runWorker({ "--socket", controlPath, "--fd-socket", fdPath });
        QVERIFY2(result.completed, qPrintable(result.processError));
        QCOMPARE(result.exitCode, 2);
        QVERIFY(result.output.isEmpty());
        QVERIFY2(result.error.contains(fdListener ? "failed to start control socket" : "failed to connect FD channel"),
                 result.error.constData());
    }
};

int runTestWorkerMain(int argc, char** argv)
{
    TestWorkerMain test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_main.moc"
