// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
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

/// True when the CLI run failed only because no Tesseract data is available
/// in this environment, in which case OCR integration tests must be skipped.
bool missingTessData(const QString& standardError)
{
    return standardError.contains(QStringLiteral("worker rejected the OCR job"))
        || standardError.contains(QStringLiteral("OCR failed"));
}

} // namespace

class TestToolsCliOcr : public QObject {
    Q_OBJECT

private slots:

    void writesRecognizedTextToFile()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString output = tempDir.filePath(QStringLiteral("page0.txt"));

        QString standardOutput;
        QString standardError;
        const int code = runCli(
            { QStringLiteral("ocr"), QStringLiteral(TEST_PDF_PATH), QStringLiteral("0"), QStringLiteral("-o"), output },
            &standardOutput,
            &standardError);
        if (code != 0 && missingTessData(standardError))
            QSKIP("tesseract data not available");
        QCOMPARE(code, 0);

        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.trimmed().isEmpty());
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
        const qint64 sizeBefore = QFileInfo(copy).size();

        QString standardError;
        const int code = runCli(
            { QStringLiteral("ocr"), copy, QStringLiteral("0"), QStringLiteral("-o"), copy }, nullptr, &standardError);
        if (code != 0 && missingTessData(standardError))
            QSKIP("tesseract data not available");
        QVERIFY(code != 0);
        QVERIFY(standardError.contains(QStringLiteral("[ERROR]")));
        QCOMPARE(QFileInfo(copy).size(), sizeBefore);
    }
};

QTEST_GUILESS_MAIN(TestToolsCliOcr)
#include "test_cli_ocr.moc"
