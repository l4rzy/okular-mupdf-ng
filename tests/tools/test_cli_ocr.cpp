// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

namespace {

/// Runs mupdfng-cli with the test worker. Returns the exit code with captured
/// output, or -1 when the process could not complete.
int runCli(const QStringList& arguments, QString* standardOutput = nullptr, QString* standardError = nullptr)
{
    QStringList full = arguments;
    full << QStringLiteral("--worker") << QStringLiteral(WORKER_BUILD_PATH);
    QProcess cli;
    cli.start(QStringLiteral(MUPDFNG_CLI_PATH), full);
    if (!cli.waitForStarted() || !cli.waitForFinished(120000))
        return -1;
    if (cli.exitStatus() != QProcess::NormalExit)
        return -1;
    if (standardOutput)
        *standardOutput = QString::fromUtf8(cli.readAllStandardOutput());
    if (standardError)
        *standardError = QString::fromUtf8(cli.readAllStandardError());
    return cli.exitCode();
}

} // namespace

class TestToolsCliOcr : public QObject {
    Q_OBJECT

private slots:

    void writesRecognizedTextToFile_data()
    {
        QTest::addColumn<int>("page");
        QTest::addColumn<QString>("expected");
        QTest::newRow("image-text") << 0 << QStringLiteral("RASTERALPHAONERASTERBETATWO");
        QTest::newRow("partial-text-layer") << 1 << QStringLiteral("UNCOVEREDSECONDLINE");
        QTest::newRow("complete-text-layer") << 2 << QString();
        QTest::newRow("native-text-only") << 3 << QString();
    }

    void writesRecognizedTextToFile()
    {
        QFETCH(int, page);
        QFETCH(QString, expected);
        if (!QFile::exists(QStringLiteral(TESSDATA_DIR "/eng.traineddata")))
            QSKIP("English traineddata is not installed");
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString output = tempDir.filePath(QStringLiteral("page0.txt"));

        QString standardOutput;
        QString standardError;
        const int code = runCli({ QStringLiteral("ocr"),
                                  QStringLiteral(TEST_PDF_PATH),
                                  QString::number(page),
                                  QStringLiteral("-o"),
                                  output },
                                &standardOutput,
                                &standardError);
        QVERIFY2(code == 0, qPrintable(standardError));

        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QString recognized = content;
        recognized.remove(QRegularExpression(QStringLiteral("\\s+")));
        QCOMPARE(recognized, expected);
        // Text-only lines: no quad-coordinate prefix from the old box format.
        for (const QString& line : content.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            QVERIFY(!line.contains(QRegularExpression(QStringLiteral("^[0-9.]+ [0-9.]+ [0-9.]+ [0-9.]+ "))));
        QVERIFY(standardOutput.contains(QStringLiteral("[OK]")));
        QVERIFY(standardOutput.contains(output));
    }

    void refusesToClobberTheInputFile()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString copy = tempDir.filePath(QStringLiteral("copy.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(TEST_PDF_PATH), copy));
        QFile file(copy);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray before = file.readAll();
        file.close();

        QString standardError;
        const int code = runCli(
            { QStringLiteral("ocr"), copy, QStringLiteral("0"), QStringLiteral("-o"), copy }, nullptr, &standardError);
        QVERIFY(code != 0);
        QVERIFY(standardError.contains(QStringLiteral("output must not be the input file")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), before);
    }
};

QTEST_GUILESS_MAIN(TestToolsCliOcr)
#include "test_cli_ocr.moc"
