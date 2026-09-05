// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ocr/ocr.hpp"
#include "genpdf.hpp"

#include <QFile>
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
#ifdef MUPDF_HAS_OCR
        m_scannedPath = m_tempDir.filePath("scanned.pdf");
        createScannedPDF(context, m_textPath, m_scannedPath);
#endif
        fz_drop_context(context);
    }

    void testInvalidInputIsRejected()
    {
        const auto result = Mu::Worker::Engine::runOcr(-1, { }, 0, "eng", 225.0f);
        QCOMPARE(result.status, Mu::Model::OcrStatus::Failed);
        QVERIFY(result.boxes.empty());
    }

#ifdef MUPDF_HAS_OCR
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
#endif
};

int runTestWorkerOcr(int argc, char** argv)
{
    TestOcr test;
    return QTest::qExec(&test, argc, argv);
}

#include "ocr.moc"
