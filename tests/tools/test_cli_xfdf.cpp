// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#include <cmath>
#include <initializer_list>

#include "../support/xfdf_fixture.hpp"

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
/// the process could not complete. Captured standard output/error are appended
/// to the out parameters when they are not null.
int runCli(const QStringList& arguments, QString* standardOutput = nullptr, QString* standardError = nullptr)
{
    QStringList full = arguments;
    full << QStringLiteral("--worker") << QStringLiteral(WORKER_BUILD_PATH);
    QProcess cli;
    cli.start(QStringLiteral(MUPDFNG_CLI_PATH), full);
    if (!cli.waitForStarted() || !cli.waitForFinished(60000))
        return -1;
    if (cli.exitStatus() != QProcess::NormalExit)
        return -1;
    if (standardOutput)
        *standardOutput = QString::fromUtf8(cli.readAllStandardOutput());
    if (standardError)
        *standardError = QString::fromUtf8(cli.readAllStandardError());
    return cli.exitCode();
}

bool coordinatesMatch(const QString& value, std::initializer_list<double> expected)
{
    const QStringList parts = value.split(QLatin1Char(','));
    if (parts.size() != static_cast<qsizetype>(expected.size()))
        return false;
    qsizetype index = 0;
    for (const double expectedCoordinate : expected) {
        bool ok = false;
        const double coordinate = parts.at(index++).toDouble(&ok);
        if (!ok || std::abs(coordinate - expectedCoordinate) > 0.001)
            return false;
    }
    return true;
}

} // namespace

class TestToolsCliXfdf : public QObject {
    Q_OBJECT

private slots:

    void checksPdfRotation_data() { addXfdfRotationRows(); }

    void checksPdfRotation()
    {
        QFETCH(int, rotation);
        QFETCH(bool, inherited);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, rotation, inherited));
        const QByteArray original = readFile(pdf);
        const QString xfdf = directory.filePath("input.xfdf");
        QFile input(xfdf);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write("<xfdf><annots><text page=\"0\" rect=\"20,30,40,50\">"
                    "<contents>imported note</contents></text></annots></xfdf>");
        input.close();

        for (const bool importing : { false, true }) {
            for (const bool existing : { false, true }) {
                const QString output = directory.filePath(importing ? "output.pdf" : "output.xfdf");
                if (existing) {
                    QFile destination(output);
                    QVERIFY(destination.open(QIODevice::WriteOnly));
                    destination.write("preserve destination");
                }
                QString error;
                const QStringList arguments =
                    importing ? QStringList { "import", pdf, xfdf, output } : QStringList { "export", pdf, output };
                const int code = runCli(arguments, nullptr, &error);
                QVERIFY2(code >= 0, qPrintable(error));
                QCOMPARE(code == 0, accepted);
                QCOMPARE(readFile(pdf), original);
                if (accepted) {
                    QVERIFY(!readFile(output).isEmpty());
                    QVERIFY(readFile(output) != QByteArray("preserve destination"));
                } else {
                    QVERIFY2(error.contains("rotated PDF pages"), qPrintable(error));
                    if (existing)
                        QCOMPARE(readFile(output), QByteArray("preserve destination"));
                    else
                        QVERIFY(!QFile::exists(output));
                }
                if (QFile::exists(output))
                    QVERIFY(QFile::remove(output));
            }
        }
    }

    void flattensPdfThroughCli()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString annotated = directory.filePath("annotated.pdf");
        const QString xfdf = directory.filePath("notes.xfdf");
        QFile notes(xfdf);
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write("<xfdf xmlns=\"http://ns.adobe.com/xfdf/\"><annots>"
                    "<square page=\"0\" rect=\"72,72,144,144\" color=\"#FF0000\"/>"
                    "</annots></xfdf>");
        notes.close();
        QString error;
        QCOMPARE(runCli({ "import", QStringLiteral(TEST_PDF_PATH), xfdf, annotated }, nullptr, &error), 0);
        const QString before = directory.filePath("before.xfdf");
        QCOMPARE(runCli({ "export", annotated, before }, nullptr, &error), 0);
        QVERIFY(countAnnotations(readFile(before)) > 0);
        const QString flattened = directory.filePath("flattened.pdf");
        QVERIFY2(runCli({ "export", annotated, flattened, "--flatten", "--pages", "0" }, nullptr, &error) == 0,
                 qPrintable(error));
        QVERIFY(readFile(flattened).startsWith("%PDF-"));
        const QString after = directory.filePath("after.xfdf");
        QCOMPARE(runCli({ "export", flattened, after }, nullptr, &error), 0);
        QCOMPARE(countAnnotations(readFile(after)), 0);
        QCOMPARE(runCli({ "export", annotated, before }, nullptr, &error), 0);
        QVERIFY(countAnnotations(readFile(before)) > 0);

        const QByteArray destination = readFile(flattened);
        QVERIFY(runCli({ "export", annotated, flattened, "--flatten", "--pages", "999999" }, nullptr, &error) != 0);
        QCOMPARE(readFile(flattened), destination);
    }

    void exportsPdfAnnotationsAsWellFormedXfdf()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString outputPath = tempDir.filePath(QStringLiteral("annotations.xfdf"));

        QProcess cli;
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH),
                  { QStringLiteral("export"),
                    QStringLiteral(TEST_PDF_PATH),
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

        QString importOutput;
        QCOMPARE(runCli({ QStringLiteral("import"), QStringLiteral(TEST_PDF_PATH), baseXfdf, firstPdf }, &importOutput),
                 0);
        QVERIFY(importOutput.contains(QStringLiteral("[INFO] Parsed 2 annotations from")));
        QVERIFY(importOutput.contains(QStringLiteral("[OK] Applied 2 annotations to")));

        QString exportOutput;
        QCOMPARE(runCli({ QStringLiteral("export"), firstPdf, exportedXfdf }, &exportOutput), 0);
        QCOMPARE(countAnnotations(readFile(exportedXfdf)), 2);
        QVERIFY(exportOutput.contains(QStringLiteral("[INFO] Found 2 annotations in")));
        QVERIFY(exportOutput.contains(QStringLiteral("[OK] Exported 2 annotations to")));

        // Re-applying the exported XFDF to a clean document reproduces both.
        QCOMPARE(runCli({ QStringLiteral("import"), QStringLiteral(TEST_PDF_PATH), exportedXfdf, secondPdf }), 0);
        QCOMPARE(runCli({ QStringLiteral("export"), secondPdf, roundTripXfdf }), 0);
        QCOMPARE(countAnnotations(readFile(roundTripXfdf)), 2);
    }

    void freeTextCalloutRoundTripsWithoutExpandingItsRect()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString inputXfdf = tempDir.filePath(QStringLiteral("callout.xfdf"));
        const QString importedPdf = tempDir.filePath(QStringLiteral("callout.pdf"));
        const QString outputXfdf = tempDir.filePath(QStringLiteral("exported.xfdf"));

        QFile source(inputXfdf);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\"><annots>"
            "<freetext page=\"0\" rect=\"72,500,150,550\" callout=\"0,792,60,550\" "
            "intent=\"FreeTextCallout\" color=\"#000000\"><contents>callout note</contents></freetext>"
            "</annots></xfdf>"));
        source.close();

        QCOMPARE(runCli({ QStringLiteral("import"), QStringLiteral(TEST_PDF_PATH), inputXfdf, importedPdf }), 0);
        QCOMPARE(runCli({ QStringLiteral("export"), importedPdf, outputXfdf }), 0);

        QXmlStreamReader reader(readFile(outputXfdf));
        bool foundCallout = false;
        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != QLatin1String("freetext"))
                continue;
            const auto attributes = reader.attributes();
            QVERIFY(coordinatesMatch(attributes.value(QLatin1String("rect")).toString(), { 72, 500, 150, 550 }));
            QVERIFY(coordinatesMatch(attributes.value(QLatin1String("callout")).toString(), { 0, 792, 60, 550 }));
            QCOMPARE(attributes.value(QLatin1String("intent")).toString(), QStringLiteral("FreeTextCallout"));
            foundCallout = true;
        }
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        QVERIFY(foundCallout);
    }

    void ignoresPdfCalloutWithoutCalloutIntent()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString outputPath = tempDir.filePath(QStringLiteral("annotations.xfdf"));
        const QString importedPdf = tempDir.filePath(QStringLiteral("imported.pdf"));
        const QString roundTripPath = tempDir.filePath(QStringLiteral("roundtrip.xfdf"));
        QCOMPARE(runCli({ QStringLiteral("export"), QStringLiteral(TEST_CALLOUT_PDF_PATH), outputPath }), 0);

        QXmlStreamReader reader(readFile(outputPath));
        bool foundCallout = false;
        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != QLatin1String("freetext"))
                continue;
            const auto attributes = reader.attributes();
            QVERIFY(coordinatesMatch(attributes.value(QLatin1String("rect")).toString(), { 72, 500, 150, 550 }));
            QVERIFY(!attributes.hasAttribute(QLatin1String("callout")));
            QVERIFY(!attributes.hasAttribute(QLatin1String("intent")));
            foundCallout = true;
        }
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        QVERIFY(foundCallout);

        QCOMPARE(runCli({ QStringLiteral("import"), QStringLiteral(TEST_PDF_PATH), outputPath, importedPdf }), 0);
        QCOMPARE(runCli({ QStringLiteral("export"), importedPdf, roundTripPath }), 0);
        QVERIFY(!readFile(roundTripPath).contains(QByteArrayLiteral(" callout=")));
    }

    void preservesInkWidthThroughPdfRoundTrip()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString inputXfdf = tempDir.filePath(QStringLiteral("ink.xfdf"));
        const QString importedPdf = tempDir.filePath(QStringLiteral("ink.pdf"));
        const QString outputXfdf = tempDir.filePath(QStringLiteral("exported.xfdf"));

        QFile source(inputXfdf);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArrayLiteral(
            "<xfdf><annots><ink page=\"0\" rect=\"100,600,140,640\" width=\"2\" color=\"#ff5500\" opacity=\"1\">"
            "<inklist><gesture>100,600;110,610;120,620;130,630;140,640</gesture></inklist>"
            "</ink></annots></xfdf>"));
        source.close();

        QCOMPARE(runCli({ QStringLiteral("import"), QStringLiteral(TEST_PDF_PATH), inputXfdf, importedPdf }), 0);
        QCOMPARE(runCli({ QStringLiteral("export"), importedPdf, outputXfdf }), 0);

        QXmlStreamReader reader(readFile(outputXfdf));
        bool foundInk = false;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == QLatin1String("ink")) {
                QCOMPARE(reader.attributes().value(QLatin1String("width")).toString(), QStringLiteral("2"));
                foundInk = true;
            }
        }
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
        QVERIFY(foundInk);
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

        QString standardError;
        QVERIFY(runCli({ QStringLiteral("import"), QStringLiteral(TEST_PDF_PATH), badXfdf, output },
                       nullptr,
                       &standardError)
                != 0);
        QVERIFY(!QFile::exists(output));
        QVERIFY(standardError.contains(QStringLiteral("[ERROR]")));
    }
};

QTEST_GUILESS_MAIN(TestToolsCliXfdf)
#include "test_cli_xfdf.moc"
