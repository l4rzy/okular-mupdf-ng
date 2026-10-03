#include "engine/pdf/document.hpp"
#include "generator/proxy/annotation.hpp"
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
#include <cstdlib>
#include <optional>

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
    }

    void init() { QVERIFY2(m_client.start(QStringLiteral(WORKER_BUILD_PATH)), "worker IPC unavailable"); }

    // Each case owns its worker, including unfinished asynchronous jobs.
    void cleanup() { m_client.stop(); }

    void cleanupTestCase() { ::Mu::Plugin::Caching::clearRootForTesting(); }

    // End-to-end: a document MuPDF had to repair is reported across IPC.
    // End-to-end: the Okular paper color reaches the worker renderer.
    void layersFlowThroughIpc()
    {
        using namespace Mu::Model;
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(QStringLiteral(TEST_SIGNATURE_PDF_DIR "/layers.pdf"), { }, pages), OpenStatus::Success);
        const auto original = m_client.layers();
        QVERIFY(original);
        QCOMPARE(original->entries.size(), std::size_t(9));
        const auto blue = std::find_if(original->entries.begin(), original->entries.end(), [](const auto& e) {
            return e.name == "Blue (initially off)";
        });
        QVERIFY(blue != original->entries.end());
        QVERIFY(!blue->selected);
        const auto image = m_client.render(0, 612, 792);
        QVERIFY(!image.isNull());
        const auto changed = m_client.setLayer({ original->generation, blue->id, true });
        QVERIFY(changed);
        QVERIFY(changed->entries[static_cast<std::size_t>(blue->id)].selected);
        QVERIFY(imageHash(image) != imageHash(m_client.render(0, 612, 792)));
        QVERIFY(!m_client.setLayer({ original->generation + 1, blue->id, false }));
        const auto locked =
            std::find_if(original->entries.begin(), original->entries.end(), [](const auto& e) { return e.locked; });
        QVERIFY(locked != original->entries.end());
        QVERIFY(!m_client.setLayer({ original->generation, locked->id, false }));
        QVERIFY(m_client.layers()->entries[static_cast<std::size_t>(blue->id)].selected);
        QVERIFY(m_client.close());
        QCOMPARE(m_client.open(QStringLiteral(TEST_SIGNATURE_PDF_DIR "/layers.pdf"), { }, pages), OpenStatus::Success);
        const auto reopened = m_client.layers();
        QVERIFY(reopened);
        QVERIFY(reopened->generation != original->generation);
        QVERIFY(!reopened->entries[static_cast<std::size_t>(blue->id)].selected);
        QVERIFY(!m_client.setLayer({ original->generation, blue->id, true }));
        QVERIFY(m_client.close());
        QCOMPARE(m_client.open(m_epub, { }, pages, DocumentType::Epub), OpenStatus::Success);
        const auto epubLayers = m_client.layers();
        QVERIFY(epubLayers);
        QVERIFY(epubLayers->entries.empty());
        QVERIFY(m_client.close());
    }

    void textExtractionReportsSuccess_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<bool>("expectedSuccess");
        QTest::newRow("text") << 0 << true;
        QTest::newRow("blank") << 1 << true;
        QTest::newRow("invalid-page") << 2 << false;
        QTest::newRow("disconnected") << 3 << false;
    }

    void textExtractionReportsSuccess()
    {
        QFETCH(int, scenario);
        QFETCH(bool, expectedSuccess);
        bool success = true;
        if (scenario == 3) {
            ::Mu::Plugin::WorkerClient disconnected;
            QVERIFY(disconnected.getTextBoxesForPage(0, 72, 72, true, &success).empty());
            QVERIFY(!success);
            return;
        }
        QString path = m_pdf;
        if (scenario == 1) {
            path = m_fixtureRoot.filePath(QStringLiteral("blank.pdf"));
            fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
            QVERIFY(context);
            createMultiPagePDF(context, path, 1);
            fz_drop_context(context);
        }
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(path, { }, pages), ::Mu::Model::OpenStatus::Success);
        const auto boxes = m_client.getTextBoxesForPage(scenario == 2 ? -1 : 0, 72, 72, true, &success);
        QCOMPARE(success, expectedSuccess);
        QCOMPARE(boxes.empty(), scenario != 0);
        QVERIFY(m_client.close());
    }

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

        // Restore defaults and close: later slots must not inherit the
        // custom paper color or an open document from this test.
        QVERIFY(m_client.setSettings(::Mu::Model::DocumentSettings { }));
        QVERIFY(m_client.close());
    }

    void annotationRemovalNotifiesChangedPage()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_pdf, { }, pages), ::Mu::Model::OpenStatus::Success);

        QList<int> changedPages;
        ::Mu::Generator::Proxy::Annotation proxy(&m_client, [&](int page) { changedPages.append(page); });
        proxy.setAvailable(true);

        const QImage before = m_client.render(0, 160, 160);
        QVERIFY(!before.isNull());

        Okular::TextAnnotation annotation;
        annotation.setTextType(Okular::TextAnnotation::Linked);
        annotation.setContents(QStringLiteral("temporary annotation"));
        annotation.setBoundingRectangle(Okular::NormalizedRect(0.2, 0.2, 0.4, 0.4));
        proxy.notifyAddition(&annotation, 0);
        QCOMPARE(changedPages, QList<int> { 0 });
        QVERIFY(annotation.nativeId().isValid());

        const QImage withAnnotation = m_client.render(0, 160, 160);
        QVERIFY(!withAnnotation.isNull());
        QVERIFY(imageHash(withAnnotation) != imageHash(before));

        changedPages.clear();
        proxy.notifyRemoval(&annotation, 0);
        QCOMPARE(changedPages, QList<int> { 0 });
        QVERIFY(!annotation.nativeId().isValid());

        // Removal is successful in the worker, and the callback identifies
        // exactly the page whose cached Okular image must be invalidated.
        const QImage afterRemoval = m_client.render(0, 160, 160);
        QVERIFY(!afterRemoval.isNull());
        QCOMPARE(imageHash(afterRemoval), imageHash(before));

        // A second removal has no native handle and must not request another
        // page refresh.
        proxy.notifyRemoval(&annotation, 0);
        QCOMPARE(changedPages, QList<int> { 0 });
        QVERIFY(m_client.close());
    }

    void renderFittedFrameStaysWithinBudget()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_pdf, { }, pages), ::Mu::Model::OpenStatus::Success);

        // This exceeds the worker's 128 MiB frame budget, so the transport
        // returns the fitted frame as-is; the generator normalizes it later.
        const QImage image = m_client.render(0, 6000, 6000);
        QVERIFY(!image.isNull());
        QVERIFY(image.width() < 6000 && image.height() < 6000);
        QVERIFY(image.sizeInBytes() <= 128 * 1024 * 1024);

        QVERIFY(m_client.close());
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
        QVERIFY(m_client.close());
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
        // The worker binary reports its MuPDF version via the ping handshake.
        QCOMPARE(m_client.engineVersion(), std::string(FZ_VERSION));

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
        QVERIFY(m_client.savePdfToFile(outputPath, { 0 }));
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QVERIFY(output.read(5) == "%PDF-");

        QVERIFY(m_client.close());
    }

    void pdfOutputIsAtomicAndIncludesLiveAnnotations_data()
    {
        QTest::addColumn<bool>("flatten");
        QTest::newRow("flatten") << true;
        QTest::newRow("print") << false;
    }

    void pdfOutputIsAtomicAndIncludesLiveAnnotations()
    {
        QFETCH(bool, flatten);
        QList<::Mu::Model::PageInfo> pages;
        QCOMPARE(m_client.open(m_pdf, QString(), pages), ::Mu::Model::OpenStatus::Success);
        ::Mu::Model::Annotation annotation;
        annotation.subtype = ::Mu::Model::AnnotationType::Highlight;
        annotation.uuid = "ipc-pdf-output";
        annotation.flags = ::Mu::Model::annotationFlagValue(::Mu::Model::AnnotationFlag::Print);
        annotation.x0 = .1;
        annotation.y0 = .3;
        annotation.x1 = .4;
        annotation.y1 = .35;
        annotation.color = 0xffffff00U;
        annotation.extras.quads.push_back({ { .1, .3 }, { .4, .3 }, { .4, .35 }, { .1, .35 } });
        const auto handle = m_client.addAnnotation(0, annotation);
        QVERIFY(handle.has_value());
        const QImage edited = m_client.render(0, 612, 792);
        QVERIFY(!edited.isNull());
        const auto expected = imageHash(edited);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString target = directory.filePath("flattened.pdf");
        QFile sentinel(target);
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        sentinel.write("existing destination");
        sentinel.close();
        const auto savePdf = [&](const QVector<int>& selectedPages) {
            return flatten ? m_client.flattenPdfToFile(target, selectedPages)
                           : m_client.savePdfToFile(target, selectedPages);
        };
        QVERIFY(!savePdf({ -1 }));
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("existing destination"));
        sentinel.close();
        QVERIFY(savePdf({ }));
        QCOMPARE(imageHash(m_client.render(0, 612, 792)), expected);
        QVERIFY(m_client.removeAnnotation(0, QString::fromStdString(handle->value)));
        QVERIFY(m_client.close());
        QCOMPARE(m_client.open(target, QString(), pages), ::Mu::Model::OpenStatus::Success);
        QVERIFY(pages.front().annotations.empty());
        QVERIFY(pages.front().formFields.empty());
        const QImage flattened = m_client.render(0, 612, 792);
        QCOMPARE(flattened.size(), edited.size());
        QCOMPARE(flattened.sizeInBytes(), edited.sizeInBytes());
        QVERIFY(std::equal(flattened.constBits(),
                           flattened.constBits() + flattened.sizeInBytes(),
                           edited.constBits(),
                           [](auto actual, auto expected) {
                               return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 2;
                           }));
        QVERIFY(m_client.close());
    }

    // Closing the document abandons the awaiting export: no completion signal,
    // no target file, and the worker's late notification stays silent.
    void epubExportAbandonedOnDocumentClose()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_epub, { }, pages, ::Mu::Model::DocumentType::Epub), ::Mu::Model::OpenStatus::Success);
        QVERIFY(!pages.isEmpty());

        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        const QString outputPath = outputDirectory.filePath(QStringLiteral("export-abandoned.pdf"));
        QSignalSpy spy(&m_client, &::Mu::Plugin::WorkerClient::pdfExportFinished);
        const auto job = m_client.startPdfExport(outputPath, { });
        QVERIFY(job.has_value());
        QVERIFY(m_client.close());

        // The job may still be running in the worker; wait out the export and
        // any subsequent timeout window to prove the signal never arrives and
        // the target file never appears.
        const auto hasAbandonedJob = [&]() {
            return std::any_of(spy.cbegin(), spy.cend(), [&](const QList<QVariant>& arguments) {
                return arguments.at(0).toULongLong() == *job;
            });
        };
        for (int attempt = 0; attempt < 10 && !hasAbandonedJob(); ++attempt)
            spy.wait(100);
        QVERIFY2(!hasAbandonedJob(), "abandoned export must not deliver a completion signal");

        QFile output(outputPath);
        QVERIFY(!output.exists());

        // A fresh export after the boundary works again once the abandoned
        // job's slot frees (the thread runs to completion in the worker).
        QCOMPARE(m_client.open(m_epub, { }, pages, ::Mu::Model::DocumentType::Epub), ::Mu::Model::OpenStatus::Success);
        std::optional<quint64> resumed;
        for (int attempt = 0; attempt < 50 && !(resumed = m_client.startPdfExport(outputPath, { })).has_value();
             ++attempt)
            QTest::qWait(100);
        QVERIFY2(resumed.has_value(), "worker did not free the export slot after the abandoned job");
        QVERIFY(m_client.close());
    }

    // A failed export submission must not queue an orphan descriptor: opening
    // the source happens before any FD send, so the FD channel stays clean and
    // subsequent file operations keep working.
    void failedExportSubmitKeepsFdChannelClean()
    {
        // Copy the corpus EPUB so its path can be deleted for this test.
        const QString sourceCopy = m_fixtureRoot.filePath(QStringLiteral("export-source.epub"));
        QVERIFY(QFile::copy(m_epub, sourceCopy));
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(sourceCopy, { }, pages, ::Mu::Model::DocumentType::Epub),
                 ::Mu::Model::OpenStatus::Success);
        QVERIFY(!pages.isEmpty());

        // Deleting the source path makes the submit's source-open fail after
        // the session is established; in-flight descriptors are unaffected.
        QVERIFY(QFile::remove(sourceCopy));

        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        QSignalSpy spy(&m_client, &::Mu::Plugin::WorkerClient::pdfExportFinished);
        QVERIFY(!m_client.startPdfExport(outputDirectory.filePath(QStringLiteral("export.pdf")), { }).has_value());

        // The FD channel must be clean: a synchronous FD-based operation works.
        QVERIFY(m_client.savePdfToFile(outputDirectory.filePath(QStringLiteral("print.pdf")), { }));
        QVERIFY(QFile(outputDirectory.filePath(QStringLiteral("print.pdf"))).open(QIODevice::ReadOnly));

        // No export notification was queued by the failed submission.
        QVERIFY2(spy.isEmpty(), "failed submit must not emit a completion signal");
        QVERIFY(m_client.close());
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

    void epubExportPdfOverIpc_data()
    {
        QTest::addColumn<bool>("withReferences");
        QTest::newRow("print") << false;
        QTest::newRow("export") << true;
    }

    void epubExportPdfOverIpc()
    {
        QFETCH(bool, withReferences);
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_epub, { }, pages, ::Mu::Model::DocumentType::Epub), ::Mu::Model::OpenStatus::Success);
        QVERIFY(!pages.isEmpty());

        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        const QString outputPath = outputDirectory.filePath(QStringLiteral("export.pdf"));
        QVERIFY(m_client.savePdfToFile(outputPath, { }, withReferences));

        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QVERIFY(output.read(5) == "%PDF-");
        ::Mu::Worker::Engine::PdfDocument exported;
        std::string error;
        QVERIFY2(exported.openFd(::dup(output.handle()), "export.pdf", &error), error.c_str());
        output.close();
        QCOMPARE(exported.pageCount(), pages.size());
        QCOMPARE(!exported.outline(&error).empty(), withReferences);
        if (!withReferences) {
            for (int page = 0; page < exported.pageCount(); ++page)
                QVERIFY(exported.extractLinks(page, &error).empty());
        }

        QVERIFY(m_client.close());
    }

    // End-to-end: async EPUB export submits immediately, the document stays
    // usable while the background job runs, and the target file appears when
    // the completion notification finalizes it.
    void epubExportPdfAsyncOverIpc()
    {
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(m_epub, { }, pages, ::Mu::Model::DocumentType::Epub), ::Mu::Model::OpenStatus::Success);
        QVERIFY(!pages.isEmpty());

        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        const QString outputPath = outputDirectory.filePath(QStringLiteral("export-async.pdf"));
        QSignalSpy spy(&m_client, &::Mu::Plugin::WorkerClient::pdfExportFinished);
        const auto job = m_client.startPdfExport(outputPath, { });
        QVERIFY(job.has_value());

        // The session document stays usable while the export job runs.
        QVERIFY(!m_client.render(0, 100, 100).isNull());

        const auto hasJob = [&]() {
            return std::any_of(spy.cbegin(), spy.cend(), [&](const QList<QVariant>& arguments) {
                return arguments.at(0).toULongLong() == *job && arguments.at(1).toBool();
            });
        };
        // Arrival, not speed: keep the budget generous for loaded CI.
        for (int attempt = 0; attempt < 50 && !hasJob(); ++attempt)
            spy.wait(100);
        QVERIFY2(hasJob(), "Timed out waiting for async PDF export completion");

        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QVERIFY(output.read(5) == "%PDF-");
        ::Mu::Worker::Engine::PdfDocument exported;
        std::string error;
        QVERIFY2(exported.openFd(::dup(output.handle()), "export.pdf", &error), error.c_str());
        output.close();
        QCOMPARE(exported.pageCount(), pages.size());

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
        // OCR latency is engine- and load-dependent; the assertion is
        // arrival, not speed, so the budget stays generous for loaded CI.
        for (int attempt = 0; attempt < 50 && !hasJob(); ++attempt)
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
