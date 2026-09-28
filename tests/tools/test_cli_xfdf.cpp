// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

namespace {

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return { };
    return file.readAll();
}

/// Counts the annotation elements that are direct children of <annots>.
int countAnnotations(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    int count = 0;
    int depth = 0;
    bool inAnnots = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QLatin1String("annots")) {
                inAnnots = true;
                depth = 0;
            } else if (inAnnots) {
                if (depth == 0)
                    ++count;
                ++depth;
            }
        } else if (reader.isEndElement()) {
            if (reader.name() == QLatin1String("annots"))
                inAnnots = false;
            else if (inAnnots && depth > 0)
                --depth;
        }
    }
    return count;
}

/// Runs mupdfng-cli with the test worker and returns its exit code, or -1 when
/// the process could not complete.
int runCli(const QStringList& arguments)
{
    QStringList full = arguments;
    full << QStringLiteral("--worker") << QStringLiteral(WORKER_BUILD_PATH);
    QProcess cli;
    cli.start(QStringLiteral(MUPDFNG_CLI_PATH), full);
    if (!cli.waitForStarted() || !cli.waitForFinished(60000))
        return -1;
    if (cli.exitStatus() != QProcess::NormalExit)
        return -1;
    return cli.exitCode();
}

} // namespace

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

    void appliesXfdfAndRoundTripsThroughExport()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString baseXfdf = tempDir.filePath(QStringLiteral("base.xfdf"));
        const QString firstPdf = tempDir.filePath(QStringLiteral("first.pdf"));
        const QString exportedXfdf = tempDir.filePath(QStringLiteral("exported.xfdf"));
        const QString secondPdf = tempDir.filePath(QStringLiteral("second.pdf"));
        const QString roundTripXfdf = tempDir.filePath(QStringLiteral("roundtrip.xfdf"));

        // Two addable annotations on page 0 in PDF user-space coordinates.
        QFile source(baseXfdf);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n<annots>\n"
            "<text page=\"0\" rect=\"72,72,200,120\" color=\"#ff0000\" opacity=\"1\" title=\"tester\">"
            "<contents>hello</contents></text>\n"
            "<highlight page=\"0\" rect=\"72,200,300,220\" color=\"#ffff00\" opacity=\"1\" "
            "coords=\"72,220,300,220,72,200,300,200\"/>\n"
            "</annots>\n</xfdf>\n"));
        source.close();

        QCOMPARE(runCli({ QStringLiteral("apply-xfdf"),
                          QStringLiteral(TEST_PDF_PATH),
                          QStringLiteral("--xfdf"),
                          baseXfdf,
                          QStringLiteral("-o"),
                          firstPdf }),
                 0);

        QCOMPARE(runCli({ QStringLiteral("export-xfdf"), firstPdf, QStringLiteral("-o"), exportedXfdf }), 0);
        QCOMPARE(countAnnotations(readFile(exportedXfdf)), 2);

        // Re-applying the exported XFDF to a clean document reproduces both.
        QCOMPARE(runCli({ QStringLiteral("apply-xfdf"),
                          QStringLiteral(TEST_PDF_PATH),
                          QStringLiteral("--xfdf"),
                          exportedXfdf,
                          QStringLiteral("-o"),
                          secondPdf }),
                 0);
        QCOMPARE(runCli({ QStringLiteral("export-xfdf"), secondPdf, QStringLiteral("-o"), roundTripXfdf }), 0);
        QCOMPARE(countAnnotations(readFile(roundTripXfdf)), 2);
    }

    void rejectsMalformedXfdf()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString badXfdf = tempDir.filePath(QStringLiteral("bad.xfdf"));
        const QString output = tempDir.filePath(QStringLiteral("out.pdf"));

        QFile file(badXfdf);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("<xfdf><annots><text page=\"0\"></annots></xfdf>"));
        file.close();

        QVERIFY(runCli({ QStringLiteral("apply-xfdf"),
                         QStringLiteral(TEST_PDF_PATH),
                         QStringLiteral("--xfdf"),
                         badXfdf,
                         QStringLiteral("-o"),
                         output })
                != 0);
        QVERIFY(!QFile::exists(output));
    }
};

QTEST_GUILESS_MAIN(TestToolsCliXfdf)
#include "test_cli_xfdf.moc"
