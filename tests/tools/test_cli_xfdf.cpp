// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

class TestToolsCliXfdf : public QObject {
    Q_OBJECT

private slots:

    void exportsPdfAnnotationsAsWellFormedXfdf()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString outputPath = tempDir.filePath(QStringLiteral("annotations.xfdf"));

        QProcess cli;
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH),
                  { QStringLiteral("export-xfdf"),
                    QStringLiteral(TEST_PDF_PATH),
                    QStringLiteral("-o"),
                    outputPath,
                    QStringLiteral("--worker"),
                    QStringLiteral(WORKER_BUILD_PATH) });
        QVERIFY2(cli.waitForStarted(), qPrintable(cli.errorString()));
        QVERIFY2(cli.waitForFinished(30000), qPrintable(cli.errorString()));
        QVERIFY2(cli.exitStatus() == QProcess::NormalExit && cli.exitCode() == 0,
                 qPrintable(QString::fromUtf8(cli.readAllStandardError())));

        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QXmlStreamReader reader(output.readAll());
        bool foundRoot = false;
        bool foundAnnots = false;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement()) {
                foundRoot |= reader.name() == QLatin1String("xfdf");
                foundAnnots |= reader.name() == QLatin1String("annots");
            }
        }
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        QVERIFY(foundRoot);
        QVERIFY(foundAnnots);
    }
};

QTEST_GUILESS_MAIN(TestToolsCliXfdf)
#include "test_cli_xfdf.moc"
