// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ocr/ocr.hpp"
#include "genpdf.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <fcntl.h>
#include <unistd.h>

class TestOcr : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_textPath;
    QString m_scannedPath;
    QString m_encryptedPath;

private slots:

    void initTestCase()
    {
        QVERIFY(m_tempDir.isValid());
        m_textPath = m_tempDir.filePath("text.pdf");
        m_encryptedPath = m_tempDir.filePath("encrypted.pdf");

        fz_context* context = fz_new_context(nullptr, nullptr, 64 * 1024 * 1024);
        QVERIFY(context);
        fz_register_document_handlers(context);
        createTextPDF(context, m_textPath);
        createEncryptedPDF(context, m_encryptedPath, QStringLiteral("correct-password"));
        m_scannedPath = m_tempDir.filePath("scanned.pdf");
        createScannedPDF(context, m_textPath, m_scannedPath);
        fz_drop_context(context);
    }

    void testTessdataDirectorySelection_data()
    {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("selected");
        QTest::newRow("custom-only") << QStringLiteral("customocr") << QStringLiteral("custom");
        QTest::newRow("custom-precedence") << QStringLiteral("eng") << QStringLiteral("custom");
        QTest::newRow("default-fallback") << QStringLiteral("deu") << QStringLiteral("default");
        QTest::newRow("filename-and-dpi") << QStringLiteral("eng_300dpi.traineddata") << QStringLiteral("custom");
        QTest::newRow("multiple-languages") << QStringLiteral("eng+deu") << QStringLiteral("default");
        QTest::newRow("script-model") << QStringLiteral("script/Latin") << QStringLiteral("custom");
        QTest::newRow("missing") << QStringLiteral("missing") << QStringLiteral("");
        QTest::newRow("parent-traversal") << QStringLiteral("../eng") << QStringLiteral("");
        QTest::newRow("absolute-path") << QStringLiteral("/eng") << QStringLiteral("");
        QTest::newRow("empty-component") << QStringLiteral("eng+") << QStringLiteral("");
    }

    void testTessdataDirectorySelection()
    {
        QFETCH(QString, language);
        QFETCH(QString, selected);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        for (const QString& model : { QStringLiteral("custom/customocr"),
                                      QStringLiteral("custom/eng"),
                                      QStringLiteral("custom/script/Latin"),
                                      QStringLiteral("default/eng"),
                                      QStringLiteral("default/deu") }) {
            const QString path = root.filePath(model + QStringLiteral(".traineddata"));
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        const auto result = Mu::Worker::Engine::findTessdataDirectory(
            language.toStdString(), { root.filePath("custom").toStdString(), root.filePath("default").toStdString() });
        if (selected.isEmpty())
            QVERIFY(!result);
        else {
            QVERIFY(result);
            QCOMPARE(QString::fromStdString(*result), root.filePath(selected));
        }
    }

    void testCustomTessdataOverCli()
    {
        const QString source = QStringLiteral(TESSDATA_DIR "/eng.traineddata");
        if (!QFile::exists(source))
            QSKIP("English traineddata is not installed");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QFile::copy(source, directory.filePath("customocr.traineddata")));
        const QString output = directory.filePath("ocr.txt");
        QProcess cli;
        cli.start(QStringLiteral(MUPDFNG_CLI_PATH),
                  { "ocr",
                    m_scannedPath,
                    "0",
                    "--worker",
                    QStringLiteral(WORKER_BUILD_PATH),
                    "--tessdata",
                    directory.path(),
                    "--lang",
                    "customocr",
                    "-o",
                    output });
        QVERIFY(cli.waitForStarted());
        QVERIFY(cli.waitForFinished(60'000));
        const QByteArray errors = cli.readAllStandardError();
        QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
        QVERIFY2(cli.exitCode() == 0, errors.constData());
        QFile text(output);
        QVERIFY(text.open(QIODevice::ReadOnly));
        QVERIFY(!text.readAll().trimmed().isEmpty());
    }

    void testInvalidInputIsRejected()
    {
        const auto result = Mu::Worker::Engine::runOcr(-1, { }, 0, "eng", 225.0f);
        QCOMPARE(result.status, Mu::Model::OcrStatus::Failed);
        QVERIFY(result.boxes.empty());
    }

    void testTextOnlyPageSkipsOcr()
    {
        const int fd = ::open(QFile::encodeName(m_textPath).constData(), O_RDONLY);
        QVERIFY(fd >= 0);
        const auto result = Mu::Worker::Engine::runOcr(fd, { }, 0, "eng", 225.0f);
        QCOMPARE(result.status, Mu::Model::OcrStatus::Success);
        QVERIFY(result.boxes.empty());
    }

    void testScannedPdfOcr()
    {
        QVERIFY(QFile::exists(m_scannedPath));
        const int fd = ::open(QFile::encodeName(m_scannedPath).constData(), O_RDONLY);
        QVERIFY(fd >= 0);
        const auto result = Mu::Worker::Engine::runOcr(fd, { }, 0, "eng", 225.0f);
        QCOMPARE(result.status, Mu::Model::OcrStatus::Success);
        QVERIFY(!result.boxes.empty());
        for (const Mu::Model::TextBox& box : result.boxes) {
            QVERIFY(box.left >= 0.0 && box.left <= box.right && box.right <= 1.0);
            QVERIFY(box.top >= 0.0 && box.top <= box.bottom && box.bottom <= 1.0);
        }
    }

    void testCancelledOcrReturnsNoBoxes()
    {
        const int fd = ::open(QFile::encodeName(m_scannedPath).constData(), O_RDONLY);
        QVERIFY(fd >= 0);
        Mu::Worker::Engine::CancellationCookie cancelled;
        cancelled.cancel();
        const auto result = Mu::Worker::Engine::runOcr(fd, { }, 0, "eng", 225.0f, &cancelled);
        QCOMPARE(result.status, Mu::Model::OcrStatus::Cancelled);
        QVERIFY(result.boxes.empty());
    }

    void testWrongPasswordReturnsNoBoxes()
    {
        const int fd = ::open(QFile::encodeName(m_encryptedPath).constData(), O_RDONLY);
        QVERIFY(fd >= 0);
        const auto result = Mu::Worker::Engine::runOcr(fd, "wrong-password", 0, "eng", 225.0f);
        QCOMPARE(result.status, Mu::Model::OcrStatus::Failed);
        QVERIFY(result.boxes.empty());
    }
};

int runTestWorkerOcr(int argc, char** argv)
{
    TestOcr test;
    return QTest::qExec(&test, argc, argv);
}

#include "ocr.moc"
