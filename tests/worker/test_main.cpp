// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
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
        QMap<QString, bool> controls;
        const QStringList booleanFields { "landlock",         "seccomp",       "linuxNamespace", "resourceLimits",
                                          "memoryProtection", "fullyHardened", "partiallyActive" };
        if (json) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(result.output, &error);
            QCOMPARE(error.error, QJsonParseError::NoError);
            QVERIFY(document.isObject());
            const auto object = document.object();
            QCOMPARE(object.size(), 9);
            for (const auto& field : booleanFields) {
                QVERIFY2(object.value(field).isBool(), qPrintable(field));
                controls.insert(field, object.value(field).toBool());
            }
            QVERIFY(object.value("landlockAbi").isDouble());
            const double abi = object.value("landlockAbi").toDouble();
            QVERIFY(abi >= 0);
            QCOMPARE(abi, double(object.value("landlockAbi").toInt(-1)));
            if (controls.value("landlock"))
                QVERIFY(abi >= 1);
            QVERIFY(object.value("reason").isString());
        } else {
            const auto lines = result.output.split('\n');
            QCOMPARE(lines.front(), QByteArray("Sandbox check on host:"));
            QMap<QString, QByteArray> values;
            for (const auto& line : lines.mid(1)) {
                if (line.isEmpty())
                    continue;
                const qsizetype separator = line.indexOf(": ");
                QVERIFY2(separator > 0, line.constData());
                values.insert(QString::fromUtf8(line.first(separator).trimmed()), line.mid(separator + 2));
            }
            for (const auto& field : booleanFields) {
                const auto value = values.value(field).split(' ').front();
                QVERIFY2(value == "yes" || value == "no", qPrintable(field));
                controls.insert(field, value == "yes");
            }
            const auto landlock = values.value("landlock").split(' ');
            QCOMPARE(landlock.size(), 2);
            QVERIFY(landlock.back().startsWith("abi="));
            bool validAbi = false;
            const int abi = landlock.back().mid(4).toInt(&validAbi);
            QVERIFY(validAbi);
            QVERIFY(abi >= 0);
            if (controls.value("landlock"))
                QVERIFY(abi >= 1);
        }
        // Kernel capabilities vary: derive the expected code from this report.
        const bool fullyHardened = controls.value("landlock") && controls.value("seccomp")
            && controls.value("linuxNamespace") && controls.value("resourceLimits");
        const bool partiallyActive = !fullyHardened
            && (controls.value("landlock") || controls.value("seccomp") || controls.value("linuxNamespace"));
        QCOMPARE(controls.value("fullyHardened"), fullyHardened);
        QCOMPARE(controls.value("partiallyActive"), partiallyActive);
        QCOMPARE(result.exitCode, fullyHardened ? 0 : partiallyActive ? 1 : 2);
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
