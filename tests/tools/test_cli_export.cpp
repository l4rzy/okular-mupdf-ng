// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <mupdf/fitz.h>

class TestCliExport : public QObject {
    Q_OBJECT
private slots:

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
