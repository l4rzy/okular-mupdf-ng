// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <mupdf/fitz.h>

class TestCliExport : public QObject {
    Q_OBJECT
private slots:

    void reportsSandboxStatus_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("json");
        QTest::newRow("default-human") << QString() << false;
        QTest::newRow("empty-mode") << QStringLiteral("") << true;
        QTest::newRow("builtin-json") << QStringLiteral("builtin") << true;
        QTest::newRow("unknown-mode") << QStringLiteral("unknown") << true;
#if defined(__linux__) && !MU_DISABLE_WORKER_SANDBOX
        QTest::newRow("bwrap-human") << QStringLiteral("bwrap") << false;
        QTest::newRow("bwrap-json") << QStringLiteral("bwrap") << true;
#endif // defined(__linux__) && !MU_DISABLE_WORKER_SANDBOX
    }

    void reportsSandboxStatus()
    {
        QFETCH(QString, mode);
        QFETCH(bool, json);
        const bool wrapped = mode == "bwrap";
        if (wrapped) {
            const auto bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
            if (bwrap.isEmpty())
                QSKIP("Bubblewrap is unavailable");
            QProcess probe;
            probe.start(bwrap, { "--unshare-user", "--unshare-net", "--ro-bind", "/", "/", "--", "/usr/bin/true" });
            QVERIFY(probe.waitForFinished(5000));
            const auto error = probe.readAllStandardError();
            if (error.startsWith("bwrap:")
                && (error.contains("Operation not permitted") || error.contains("Permission denied")
                    || error.contains("No permissions to create a new namespace")))
                QSKIP(error.constData());
            QCOMPARE(probe.exitCode(), 0);
        }
        QStringList arguments { "sandbox", "--worker", QStringLiteral(WORKER_BUILD_PATH) };
        if (json)
            arguments << "--json";
        auto environment = QProcessEnvironment::systemEnvironment();
        if (mode.isNull())
            environment.remove(QStringLiteral("MUPDFNG_WORKER_SANDBOX"));
        else
            environment.insert(QStringLiteral("MUPDFNG_WORKER_SANDBOX"), mode);
        QProcess cli;
        cli.setProcessEnvironment(environment);
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH), arguments);
        QVERIFY(cli.waitForFinished(30000));
        QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
        const auto output = cli.readAllStandardOutput();
        const auto errorOutput = cli.readAllStandardError();
        QVERIFY2(!output.isEmpty(), errorOutput.constData());
        QCOMPARE(errorOutput.contains("Falling back to the built-in sandbox."), mode == "unknown");
        const auto profile = wrapped ? QStringLiteral("mupdfng.linux.bwrap.v1") : QStringLiteral("mupdfng.linux.v1");
        if (json) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(output, &error);
            QCOMPARE(error.error, QJsonParseError::NoError);
            const auto report = document.object();
            QCOMPARE(report.value("platform").toString(), QStringLiteral("linux"));
            QCOMPARE(report.value("profile").toString(), profile);
            QVERIFY(report.value("diagnostics").isArray());
            QVERIFY(!report.value("diagnostics").toArray().contains("Namespaces inherited from Bubblewrap launcher"));
            QStringList ids;
            for (const auto& entry : report.value("protections").toArray()) {
                const auto protection = entry.toObject();
                ids << protection.value("id").toString();
                QVERIFY(protection.value("state").isString());
                QVERIFY(protection.value("diagnostic").isString());
                if (protection.value("id") == "linux.namespaces") {
                    QCOMPARE(protection.value("diagnostic").toString(),
                             wrapped ? QStringLiteral("Namespaces inherited from Bubblewrap launcher") : QString());
                }
            }
            QVERIFY(ids.contains("linux.seccomp"));
            QCOMPARE(ids.contains("linux.bubblewrap"), wrapped);
            const auto isolation = report.value("isolation").toString();
            QCOMPARE(cli.exitCode(), isolation == "full" ? 0 : isolation == "degraded" ? 1 : 2);
        } else {
            QVERIFY(output.startsWith("Sandbox: linux profile=" + profile.toUtf8()));
            QVERIFY(output.contains("linux.seccomp:"));
            QCOMPARE(output.contains("Namespaces inherited from Bubblewrap launcher"), wrapped);
            if (wrapped) {
                QVERIFY(output.contains("  linux.namespaces: enforced required=1 version=\n"
                                        "    Namespaces inherited from Bubblewrap launcher\n"
                                        "  linux.resource_limits:"));
            }
            QCOMPARE(output.contains("linux.bubblewrap: enforced"), wrapped);
            QCOMPARE(cli.exitCode(),
                     output.contains("isolation=full")           ? 0
                         : output.contains("isolation=degraded") ? 1
                                                                 : 2);
        }
    }

    void reportsSandboxStartupFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile worker(directory.filePath("invalid-worker"));
        QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.close();
        QProcess cli;
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH), { "sandbox", "--json", "--worker", worker.fileName() });
        QVERIFY(cli.waitForFinished(30000));
        QCOMPARE(cli.exitCode(), 2);
        QVERIFY(cli.readAllStandardOutput().isEmpty());
        QVERIFY(cli.readAllStandardError().contains("could not start the worker"));
    }

    void rejectsUnavailableBubblewrap_data()
    {
        QTest::addColumn<bool>("hasLauncher");
        QTest::newRow("missing-launcher") << false;
#if defined(__linux__) && !MU_DISABLE_WORKER_SANDBOX
        QTest::newRow("failed-launcher") << true;
#endif // defined(__linux__) && !MU_DISABLE_WORKER_SANDBOX
    }

    void rejectsUnavailableBubblewrap()
    {
        QFETCH(bool, hasLauncher);
        QTemporaryDir path;
        QVERIFY(path.isValid());
        if (hasLauncher) {
            QFile launcher(path.filePath(QStringLiteral("bwrap")));
            QVERIFY(launcher.open(QIODevice::WriteOnly));
            QVERIFY(launcher.write("#!/bin/sh\nexit 1\n") > 0);
            QVERIFY(launcher.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            launcher.close();
        }
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("MUPDFNG_WORKER_SANDBOX"), QStringLiteral("bwrap"));
        environment.insert(QStringLiteral("PATH"), path.path());
        QProcess cli;
        cli.setProcessEnvironment(environment);
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH),
                  { "sandbox", "--json", "--worker", QStringLiteral(WORKER_BUILD_PATH) });
        QVERIFY(cli.waitForFinished(10000));
        QCOMPARE(cli.exitCode(), 2);
        QVERIFY(cli.readAllStandardOutput().isEmpty());
        QVERIFY(cli.readAllStandardError().contains("could not start the worker"));
    }

    void exportsEpubToAzw3()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto output = directory.filePath(QStringLiteral("export.azw3"));
        QProcess cli;
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH),
                  { QStringLiteral("export"),
                    QStringLiteral(TEST_EPUB_PATH),
                    output,
                    QStringLiteral("--worker"),
                    QStringLiteral(WORKER_BUILD_PATH) });
        QVERIFY(cli.waitForFinished(60000));
        QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
        QVERIFY2(cli.exitCode() == 0, cli.readAllStandardError().constData());
        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll().mid(60, 8), QByteArray("BOOKMOBI"));
    }

    void exportsKindleToEpub_data()
    {
        QTest::addColumn<QString>("fixture");
        QTest::newRow("azw3") << QStringLiteral("sample.azw3");
        QTest::newRow("kfx") << QStringLiteral("sample.kfx");
    }

    void exportsKindleToEpub()
    {
        QFETCH(QString, fixture);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto output = directory.filePath(QStringLiteral("export.epub"));
        QProcess cli;
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH),
                  { QStringLiteral("export"),
                    QStringLiteral(TEST_MOBI_DIR "/") + fixture,
                    output,
                    QStringLiteral("--worker"),
                    QStringLiteral(WORKER_BUILD_PATH) });
        QVERIFY(cli.waitForFinished(60000));
        QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
        QVERIFY2(cli.exitCode() == 0, cli.readAllStandardError().constData());
        QCOMPARE(QMimeDatabase().mimeTypeForFile(output, QMimeDatabase::MatchContent).name(),
                 QStringLiteral("application/epub+zip"));
    }

    void exportsMarkdown_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("suffix");
        QTest::newRow("epub") << QStringLiteral(TEST_EPUB_PATH) << QStringLiteral("md");
        QTest::newRow("azw3") << QStringLiteral(TEST_MOBI_DIR "/sample.azw3") << QStringLiteral("markdown");
        QTest::newRow("kfx") << QStringLiteral(TEST_MOBI_DIR "/sample.kfx") << QStringLiteral("md");
    }

    void exportsMarkdown()
    {
        QFETCH(QString, path);
        QFETCH(QString, suffix);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto output = directory.filePath(QStringLiteral("export.") + suffix);
        QProcess cli;
        cli.start(
            QStringLiteral(MUPDFNG_CLI_PATH),
            { QStringLiteral("export"), path, output, QStringLiteral("--worker"), QStringLiteral(WORKER_BUILD_PATH) });
        QVERIFY(cli.waitForFinished(60000));
        QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
        QVERIFY2(cli.exitCode() == 0, cli.readAllStandardError().constData());
        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        const auto markdown = result.readAll();
        QVERIFY2(markdown.contains("Hello"), markdown.constData());
        QVERIFY2(markdown.contains("# "), markdown.constData());
        QVERIFY(!markdown.contains("<html"));
    }

    void appliesLayout_data()
    {
        QTest::addColumn<bool>("useLayout");
        QTest::newRow("worker-defaults") << false;
        QTest::newRow("okular-layout") << true;
    }

    void appliesLayout()
    {
        QFETCH(bool, useLayout);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile config(directory.filePath("okular-mupdf-ngrc"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        QVERIFY(config.write("[EPUB]\nEpubPageSize=Letter\nEpubFontSize=14\n") > 0);
        config.close();
        QProcess cli;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_CONFIG_HOME", directory.path());
        environment.insert("XDG_CONFIG_DIRS", directory.path());
        cli.setProcessEnvironment(environment);
        const auto output = directory.filePath("export.pdf");
        const QByteArray outputPath = output.toUtf8();
        QStringList arguments {
            "export", QStringLiteral(TEST_EPUB_PATH), output, "--worker", QStringLiteral(WORKER_BUILD_PATH)
        };
        if (useLayout)
            arguments.append("--use-layout");
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH), arguments);
        QVERIFY(cli.waitForFinished(60000));
        QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
        QVERIFY2(cli.exitCode() == 0, cli.readAllStandardError().constData());
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        QVERIFY(context);
        fz_document* document = nullptr;
        fz_page* page = nullptr;
        fz_rect bounds { };
        bool failed = false;
        fz_var(document);
        fz_var(page);
        fz_var(failed);
        fz_try(context)
        {
            fz_register_document_handlers(context);
            document = fz_open_document(context, outputPath.constData());
            page = fz_load_page(context, document, 0);
            bounds = fz_bound_page(context, page);
        }
        fz_always(context)
        {
            fz_drop_page(context, page);
            fz_drop_document(context, document);
        }
        fz_catch(context)
        {
            failed = true;
        }
        fz_drop_context(context);
        QVERIFY(!failed);
        if (useLayout) {
            // The worker's Letter preset uses 216 x 279 mm.
            QVERIFY(std::abs(bounds.x1 - bounds.x0 - 612.283f) < 0.1f);
            QVERIFY(std::abs(bounds.y1 - bounds.y0 - 790.866f) < 0.1f);
        } else {
            // Worker defaults are B5, independent of the persisted Letter setting.
            QVERIFY(std::abs(bounds.x1 - bounds.x0 - 498.898f) < 0.1f);
        }
    }
};

QTEST_GUILESS_MAIN(TestCliExport)
#include "test_cli_export.moc"
