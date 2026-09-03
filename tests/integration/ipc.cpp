#include "genpdf.hpp"
#include "plugin/caching/cache_file.hpp"
#include "plugin/caching/epub_cache.hpp"
#include "plugin/worker_client.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

#include "plugin/util/temp_dir.hpp"

extern "C" {
#include <mupdf/pdf.h>
}

#ifndef WORKER_BUILD_PATH
#define WORKER_BUILD_PATH ""
#endif

class TestIpc : public QObject {
    Q_OBJECT

    QTemporaryDir m_fixtureRoot;
    QTemporaryDir m_cacheRoot;
    ::Mu::Plugin::WorkerClient m_client;
    QString m_pdf;
    QString m_encryptedPdf;
    QString m_epub;

    static QByteArray imageHash(const QImage& image)
    {
        const QByteArray bytes(reinterpret_cast<const char*>(image.constBits()),
                               static_cast<qsizetype>(image.sizeInBytes()));
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    }

private slots:

    void initTestCase()
    {
        QVERIFY(m_fixtureRoot.isValid());
        QVERIFY(m_cacheRoot.isValid());

        m_pdf = m_fixtureRoot.filePath(QStringLiteral("document.pdf"));
        m_encryptedPdf = m_fixtureRoot.filePath(QStringLiteral("encrypted.pdf"));
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, m_pdf);
        createEncryptedPDF(context, m_encryptedPdf, QStringLiteral("correct-password"));
        fz_drop_context(context);
        QVERIFY(QFile::exists(m_pdf));
        QVERIFY(QFile::exists(m_encryptedPdf));

        m_epub = QStringLiteral(TEST_EPUB_DIR "/sample.epub");
        ::Mu::Plugin::Caching::setRootForTesting(m_cacheRoot.path());
        if (!m_client.start(QStringLiteral(WORKER_BUILD_PATH)))
            QSKIP("worker IPC unavailable");
    }

    void cleanupTestCase()
    {
        m_client.stop();
        ::Mu::Plugin::Caching::clearRootForTesting();
    }

    // End-to-end: a document MuPDF had to repair is reported across IPC.
    // End-to-end: the Okular paper color reaches the worker renderer.
    void paperColorFlowsThroughIpc()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_pdf, { }, pages), ::Mu::Model::OpenStatus::Success);

        ::Mu::Model::DocumentSettings settings;
        settings.paperColorRgb = 0x112233;
        QVERIFY(m_client.setSettings(settings));

        const QImage page = m_client.render(0, 100, 100);
        QVERIFY(!page.isNull());
        const QColor corner = page.pixelColor(99, 99);
        QCOMPARE(corner.red(), 0x11);
        QCOMPARE(corner.green(), 0x22);
        QCOMPARE(corner.blue(), 0x33);
    }

    void repairedDocumentStateFlowsThroughIpc()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString broken = dir.filePath(QStringLiteral("broken.pdf"));
        QVERIFY(QFile::copy(m_pdf, broken));
        QFile file(broken);
        QVERIFY(file.open(QIODevice::ReadWrite));
        const QByteArray data = file.readAll();
        const qsizetype startxref = data.lastIndexOf("startxref");
        QVERIFY(startxref > 0);
        const qsizetype digitsStart = data.indexOf('\n', startxref) + 1;
        const qsizetype digitsEnd = data.indexOf('\n', digitsStart);
        QVERIFY(digitsEnd > digitsStart);
        QByteArray corrupted = data;
        for (qsizetype i = digitsStart; i < digitsEnd; ++i)
            corrupted[i] = '9';
        file.seek(0);
        QVERIFY(file.write(corrupted) == corrupted.size());
        file.close();

        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(broken, { }, pages), ::Mu::Model::OpenStatus::Success);
        const auto metadata = m_client.getDocumentInfo({ QStringLiteral("repaired") });
        QCOMPARE(metadata.values.at("repaired"), std::string("true"));
    }

    void pdfTransportPreservesMappedFramesAndTransfersOutput()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_pdf, { }, pages), ::Mu::Model::OpenStatus::Success);
        QVERIFY(!pages.isEmpty());

        const auto metadata = m_client.getDocumentInfo();
        QCOMPARE(metadata.pageCount, pages.size());
        QVERIFY(metadata.values.contains("hash"));
        QCOMPARE(metadata.values.at("hash").size(), std::size_t(64));
        // The worker binary reports the MuPDF version it was built with.
        QCOMPARE(metadata.values.at("engineVersion"), std::string(FZ_VERSION));

        const auto sandbox = m_client.sandboxStatus();
        QVERIFY(sandbox.landlock || sandbox.seccomp || sandbox.linuxNamespace || sandbox.memoryProtection);

        QImage retained = m_client.render(0, 160, 160);
        QVERIFY(!retained.isNull());
        QCOMPARE(retained.size(), QSize(160, 160));
        const auto retainedHash = imageHash(retained);
        QVERIFY(!m_client.render(0, 160, 160).isNull());
        QCOMPARE(imageHash(retained), retainedHash);

        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        const QString outputPath = outputDirectory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(m_client.printPdfToFile(outputPath, { 0 }));
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QVERIFY(output.read(5) == "%PDF-");

        QVERIFY(m_client.close());
        QCOMPARE(imageHash(retained), retainedHash);
    }

    void epubOutlineCacheRoundTrip()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_epub, { }, pages, ::Mu::Model::DocumentType::Epub), ::Mu::Model::OpenStatus::Success);
        const auto first = m_client.synopsis();
        QVERIFY(!first.empty());

        const auto cached = ::Mu::Plugin::Caching::EPUB::Cache::load(m_epub, ::Mu::Model::DocumentSettings { });
        QVERIFY(cached);
        QVERIFY(cached->accelerator);
        QVERIFY(cached->outline);
        QCOMPARE(cached->outline->size(), first.size());
        QCOMPARE(cached->outline->front().title, first.front().title);
        QVERIFY(m_client.close());

        QCOMPARE(m_client.open(m_epub, { }, pages, ::Mu::Model::DocumentType::Epub), ::Mu::Model::OpenStatus::Success);
        const auto second = m_client.synopsis();
        QCOMPARE(second.size(), first.size());
        QCOMPARE(second.front().title, first.front().title);
        QVERIFY(m_client.close());
    }

    void ocrCompletionArrivesWhileTransportIdle()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_pdf, { }, pages), ::Mu::Model::OpenStatus::Success);
        QSignalSpy spy(&m_client, &::Mu::Plugin::WorkerClient::ocrDone);
        const auto job = m_client.startOcrPage(0, QStringLiteral("eng"), 225);
        QVERIFY(job);

        const auto hasJob = [&]() {
            return std::any_of(spy.cbegin(), spy.cend(), [&](const QList<QVariant>& arguments) {
                return arguments.at(0).toULongLong() == *job && arguments.at(1).toInt() == 0;
            });
        };
        for (int attempt = 0; attempt < 20 && !hasJob(); ++attempt)
            spy.wait(100);
        QVERIFY2(hasJob(), "Timed out waiting for idle OCR completion signal");

        const auto result = m_client.ocrResult(*job);
        QVERIFY(result.status == ::Mu::Model::OcrStatus::Success
                || result.status == ::Mu::Model::OcrStatus::Unavailable);
        QVERIFY(m_client.close());
    }

    void pooledFramesStayAvailableAtSlotLimit()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_pdf, { }, pages), ::Mu::Model::OpenStatus::Success);
        // More than the 8 pooled slots must continue through transient frames.
        for (int dimension = 24; dimension < 64; ++dimension) {
            const QImage image = m_client.render(0, dimension, dimension);
            QVERIFY2(!image.isNull(), "pooled frame fallback failed");
        }
        QVERIFY(m_client.close());
    }

    void openStatusDistinguishesPasswordAndDocumentFailures()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_encryptedPdf, { }, pages), ::Mu::Model::OpenStatus::NeedsPassword);
        QCOMPARE(m_client.open(m_encryptedPdf, QStringLiteral("wrong-password"), pages),
                 ::Mu::Model::OpenStatus::NeedsPassword);
        QCOMPARE(m_client.open(m_encryptedPdf, QStringLiteral("correct-password"), pages),
                 ::Mu::Model::OpenStatus::Success);
        QVERIFY(m_client.close());

        QCOMPARE(m_client.open(m_fixtureRoot.filePath(QStringLiteral("missing.pdf")), { }, pages),
                 ::Mu::Model::OpenStatus::Failed);
    }
};

int runTestIntegrationIpc(int argc, char** argv)
{
    TestIpc test;
    return QTest::qExec(&test, argc, argv);
}

#include "ipc.moc"
