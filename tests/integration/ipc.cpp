#include "engine/pdf/document.hpp"
#include "generator/proxy/annotation.hpp"
#include "genpdf.hpp"
#include "plugin/caching/cache_file.hpp"
#include "plugin/caching/epub_cache.hpp"
#include "plugin/caching/pdf_toc_cache.hpp"
#include "plugin/worker_client.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <future>
#include <optional>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "plugin/util/temp_dir.hpp"
#include "shared/transport/common.hpp"
#include "shared/transport/frame_buffer.hpp"
#include "sys/sys.hpp"

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

    // IPC images wrap pixels immediately after the shared frame header.
    static std::uint64_t frameRequestId(const QImage& image)
    {
        return reinterpret_cast<const Mu::IPC::FrameBufferHeader*>(image.constBits()
                                                                   - sizeof(Mu::IPC::FrameBufferHeader))
            ->requestId;
    }

    // Locate the live worker control socket without exposing transport internals
    // in the production API. Linux SO_PEERCRED identifies its process as well.
    static int workerControlSocket(pid_t& workerPid)
    {
        const QString workerPath = QFileInfo(QStringLiteral(RENDER_WORKER_BUILD_PATH)).canonicalFilePath();
        for (const auto& name :
             QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            bool ok = false;
            const int fd = name.toInt(&ok);
            int type = 0;
            socklen_t size = sizeof(type);
            if (!ok || ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM)
                continue;
            ucred peer { };
            size = sizeof(peer);
            if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0
                && QFileInfo(QStringLiteral("/proc/%1/exe").arg(peer.pid)).symLinkTarget() == workerPath) {
                workerPid = peer.pid;
                return fd;
            }
        }
        return -1;
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

    void workerRestartCountSurvivesManualStart_data()
    {
        QTest::addColumn<bool>("validBinary");
        QTest::newRow("successful-manual-start") << true;
        QTest::newRow("failed-manual-start") << false;
    }

    void workerRestartCountSurvivesManualStart()
    {
        QFETCH(bool, validBinary);
        ::Mu::Plugin::WorkerClient client;
        QCOMPARE(client.restartCount(), quint64(0));
        QVERIFY(client.start(QStringLiteral(RENDER_WORKER_BUILD_PATH)));
        QCOMPARE(client.restartCount(), quint64(0));
        QSignalSpy restarted(&client, &::Mu::Plugin::WorkerClient::workerRestarted);
        for (quint64 count = 1; count <= 2; ++count) {
            pid_t workerPid = -1;
            QVERIFY(workerControlSocket(workerPid) >= 0);
            QCOMPARE(::kill(workerPid, SIGKILL), 0);
            QTRY_COMPARE_WITH_TIMEOUT(restarted.size(), static_cast<qsizetype>(count), 5000);
            QCOMPARE(client.restartCount(), count);
            client.commitSessionReady();
        }
        client.stop();
        QCOMPARE(client.restartCount(), quint64(2));
        QCOMPARE(client.start(validBinary ? QStringLiteral(RENDER_WORKER_BUILD_PATH) : m_pdf), validBinary);
        QCOMPARE(client.restartCount(), quint64(2));
    }

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
        QCOMPARE(m_client.usesSystemMuPdf(), bool(TEST_SYSTEM_MUPDF));

        const auto sandbox = m_client.sandboxStatus();
        if constexpr (MU_DISABLE_WORKER_SANDBOX) {
            QVERIFY(!sandbox.landlock && !sandbox.seccomp && !sandbox.linuxNamespace && !sandbox.resourceLimits
                    && !sandbox.memoryProtection);
            QCOMPARE(sandbox.reason, std::string("sandbox disabled by build configuration"));
        } else {
            QVERIFY(sandbox.landlock || sandbox.seccomp || sandbox.linuxNamespace || sandbox.memoryProtection);
        }

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

    void pdfGeneratedOutlineCacheRoundTrip()
    {
        using namespace Mu::Model;
        const QString path = m_fixtureRoot.filePath("generated-outline.pdf");
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context,
                      path,
                      "BT /F1 18 Tf 72 700 Td (1 Introduction) Tj 0 -40 Td "
                      "/F1 12 Tf (Ordinary body text with enough characters to establish the normal font size.) Tj ET");
        fz_drop_context(context);
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(path, { }, pages), OpenStatus::Success);
        const auto first = m_client.synopsis();
        QCOMPARE(first.size(), std::size_t(1));
        QCOMPARE(first.front().title, std::string("1 Introduction"));
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QString cachePath = ::Mu::Plugin::Caching::PDF::tocCachePath(source.handle());
        const auto cached = ::Mu::Plugin::Caching::PDF::loadToc(cachePath, 1);
        QVERIFY(cached);
        QCOMPARE(cached->front().title, first.front().title);
        QVERIFY(m_client.close());
        // A distinguishable cached result proves reopen uses the cache rather
        // than generating the same tree again.
        auto replacement = first;
        replacement.front().title = "Cached heading";
        QVERIFY(::Mu::Plugin::Caching::PDF::saveToc(cachePath, 1, replacement));
        QCOMPARE(m_client.open(path, { }, pages), OpenStatus::Success);
        QCOMPARE(m_client.synopsis().front().title, std::string("Cached heading"));
        QVERIFY(m_client.close());
        // Empty completed scans are cache hits too.
        QVERIFY(::Mu::Plugin::Caching::PDF::saveToc(cachePath, 1, { }));
        QCOMPARE(m_client.open(path, { }, pages), OpenStatus::Success);
        QVERIFY(m_client.synopsis().empty());
        QVERIFY(m_client.close());
        // Memory-backed opens use the staged source's content identity.
        QVERIFY(source.seek(0));
        QCOMPARE(m_client.openData(source.readAll(), { }, pages), OpenStatus::Success);
        QVERIFY(m_client.synopsis().empty());
        QVERIFY(m_client.close());
    }

    void pdfHeuristicSynopsisPolicy_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("cached");
        QTest::addColumn<bool>("memorySource");
        QTest::newRow("enabled-fresh-file") << true << false << false;
        QTest::newRow("disabled-fresh-file") << false << false << false;
        QTest::newRow("enabled-cached-file") << true << true << false;
        QTest::newRow("disabled-cached-file") << false << true << false;
        QTest::newRow("enabled-fresh-data") << true << false << true;
        QTest::newRow("disabled-fresh-data") << false << false << true;
        QTest::newRow("enabled-cached-data") << true << true << true;
        QTest::newRow("disabled-cached-data") << false << true << true;
    }

    void pdfHeuristicSynopsisPolicy()
    {
        QFETCH(bool, enabled);
        QFETCH(bool, cached);
        QFETCH(bool, memorySource);
        using namespace Mu::Model;
        const QString path = m_fixtureRoot.filePath("heuristic-policy.pdf");
        ::Mu::Worker::Engine::PdfDocument document;
        createTextPDF(document.context(),
                      path,
                      "BT /F1 18 Tf 72 700 Td (1 Introduction) Tj 0 -40 Td "
                      "/F1 12 Tf (Ordinary body text with enough characters to establish the normal font size.) Tj ET");
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QString cachePath = ::Mu::Plugin::Caching::PDF::tocCachePath(source.handle());
        QVERIFY(!cachePath.isEmpty());
        QFile::remove(cachePath);
        if (cached) {
            OutlineNode node;
            node.title = "Cached heading";
            node.link.valid = true;
            node.link.viewport.page = 0;
            node.link.viewport.coordinateMask = Viewport::CoordinateX | Viewport::CoordinateY;
            QVERIFY(::Mu::Plugin::Caching::PDF::saveToc(cachePath, 1, { node }));
        }
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        const auto status =
            memorySource ? m_client.openData(source.readAll(), { }, pages) : m_client.open(path, { }, pages);
        QCOMPARE(status, OpenStatus::Success);
        bool generated = false;
        const auto outline = m_client.synopsis(enabled, &generated);
        QCOMPARE(generated, enabled);
        if (enabled) {
            QCOMPARE(outline.size(), std::size_t(1));
            QCOMPARE(outline.front().title, std::string(cached ? "Cached heading" : "1 Introduction"));
        } else {
            QVERIFY(outline.empty());
            QCOMPARE(QFile::exists(cachePath), cached);
        }
        // Changing request policy must bypass a memoized generated tree too,
        // and leave existing disk entries available for re-enabling.
        const auto restored = m_client.synopsis(true, &generated);
        QVERIFY(generated);
        QCOMPARE(restored.size(), std::size_t(1));
        QCOMPARE(restored.front().title, std::string(cached ? "Cached heading" : "1 Introduction"));
        QVERIFY(m_client.synopsis(false).empty());
        const auto reused = m_client.synopsis(true, &generated);
        QVERIFY(generated);
        QCOMPARE(reused.size(), std::size_t(1));
        QCOMPARE(reused.front().title, restored.front().title);
        QVERIFY(m_client.close());
    }

    void pdfEmbeddedOutlinePrecedesCache_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("heuristic-enabled") << true;
        QTest::newRow("heuristic-disabled") << false;
    }

    void pdfEmbeddedOutlinePrecedesCache()
    {
        QFETCH(bool, enabled);
        using namespace Mu::Model;
        const QString path = m_fixtureRoot.filePath("embedded-outline.pdf");
        ::Mu::Worker::Engine::PdfDocument document;
        createTextPDF(document.context(), path);
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY(document.openFd(::dup(source.handle()), "embedded-outline.pdf", &error));
        auto* iterator = fz_new_outline_iterator(document.context(), document.document());
        fz_outline_item item { };
        item.title = const_cast<char*>("Embedded heading");
        item.uri = const_cast<char*>("#page=1");
        fz_outline_iterator_insert(document.context(), iterator, &item);
        fz_drop_outline_iterator(document.context(), iterator);
        const QString saved = m_fixtureRoot.filePath("embedded-outline-saved.pdf");
        const int output = ::open(QFile::encodeName(saved).constData(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        QVERIFY(output >= 0);
        QVERIFY2(document.saveFd(output, &error), error.c_str());
        QFile input(saved);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QString cachePath = ::Mu::Plugin::Caching::PDF::tocCachePath(input.handle());
        OutlineNode node;
        node.title = "Cached fallback";
        node.link.valid = true;
        node.link.viewport.page = 0;
        node.link.viewport.coordinateMask = Viewport::CoordinateX | Viewport::CoordinateY;
        QVERIFY(::Mu::Plugin::Caching::PDF::saveToc(cachePath, 1, { node }));
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(saved, { }, pages), OpenStatus::Success);
        bool generated = true;
        const auto outline = m_client.synopsis(enabled, &generated);
        QVERIFY(!generated);
        QCOMPARE(outline.size(), std::size_t(1));
        QCOMPARE(outline.front().title, std::string("Embedded heading"));
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

    void cancelledRendersDrainSafely_data()
    {
        QTest::addColumn<bool>("epub");
        QTest::addColumn<int>("finish");
        QTest::newRow("pdf-next-render") << false << 0;
        QTest::newRow("epub-next-render") << true << 0;
        QTest::newRow("stop-after-cancellation") << false << 1;
        QTest::newRow("worker-failure-after-cancellation") << false << 2;
    }

    void cancellableRenderMatchesOrdinary_data()
    {
        QTest::addColumn<bool>("epub");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QRect>("tile");
        for (const bool epub : { false, true }) {
            const QByteArray prefix = epub ? "epub-" : "pdf-";
            QTest::newRow((prefix + "page").constData()) << epub << QSize(601, 803) << QRect { };
            QTest::newRow((prefix + "tile").constData()) << epub << QSize(601, 803) << QRect(57, 91, 137, 193);
            QTest::newRow((prefix + "letterboxed-tile").constData())
                << epub << QSize(803, 401) << QRect(193, 37, 211, 137);
        }
    }

    void cancellableRenderMatchesOrdinary()
    {
        QFETCH(bool, epub);
        QFETCH(QSize, size);
        QFETCH(QRect, tile);
        using namespace Mu::Model;
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(epub ? m_epub : m_pdf, { }, pages, epub ? DocumentType::Epub : DocumentType::Pdf),
                 OpenStatus::Success);
        DocumentSettings settings;
        settings.paperColorRgb = 0x112233;
        QVERIFY(m_client.setSettings(settings));
        const QImage expected = m_client.render(0, size.width(), size.height(), tile);
        const QImage actual = m_client.render(0, size.width(), size.height(), tile, [] { return false; });
        QVERIFY(!expected.isNull());
        QVERIFY(!actual.isNull());
        QCOMPARE(actual, expected);
        QVERIFY(m_client.close());
    }

    void activeRenderCancellation_data()
    {
        QTest::addColumn<bool>("epub");
        QTest::addColumn<bool>("tiled");
        QTest::newRow("pdf-page") << false << false;
        QTest::newRow("pdf-tile") << false << true;
        QTest::newRow("epub-page") << true << false;
        QTest::newRow("epub-tile") << true << true;
    }

    void activeRenderCancellation()
    {
        QFETCH(bool, epub);
        QFETCH(bool, tiled);
        const QString path =
            m_fixtureRoot.filePath(epub ? QStringLiteral("expensive.epub") : QStringLiteral("expensive.pdf"));
        QByteArray contents;
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        if (epub) {
            contents.append(
                "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body style=\"font-size:1px;line-height:1px\">");
            contents.append(
                QByteArray("<span style=\"color:blue\">i</span><span style=\"color:red\">i</span>").repeated(20000));
            contents.append("</body></html>");
            fz_archive* archive = fz_open_zip_archive(context, QFile::encodeName(m_epub).constData());
            fz_zip_writer* zip = fz_new_zip_writer(context, QFile::encodeName(path).constData());
            for (int i = 0; i < fz_count_archive_entries(context, archive); ++i) {
                const char* name = fz_list_archive_entry(context, archive, i);
                fz_buffer* buffer = std::string_view(name) == "OEBPS/chapter1.html"
                    ? fz_new_buffer_from_copied_data(context,
                                                     reinterpret_cast<const unsigned char*>(contents.constData()),
                                                     static_cast<std::size_t>(contents.size()))
                    : fz_read_archive_entry(context, archive, name);
                fz_write_zip_entry(context, zip, name, buffer, 0);
                fz_drop_buffer(context, buffer);
            }
            fz_close_zip_writer(context, zip);
            fz_drop_zip_writer(context, zip);
            fz_drop_archive(context, archive);
        } else {
            contents = QByteArray("0 0 612 792 re f\n").repeated(20000);
            createTextPDF(context, path, contents.constData());
        }
        fz_drop_context(context);
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(
            m_client.open(path, { }, pages, epub ? ::Mu::Model::DocumentType::Epub : ::Mu::Model::DocumentType::Pdf),
            ::Mu::Model::OpenStatus::Success);
        const QImage expected = m_client.render(0, 32, 32);
        QVERIFY(!expected.isNull());

        Mu::Worker::Sys::Mapping cookie;
        bool sawProgress = false;
        const auto started = std::chrono::steady_clock::now();
        const auto shouldAbort = [&] {
            if (!cookie) {
                // The caller keeps this job's cookie FD alive while checking
                // cancellation. Observe MuPDF's documented progress field to
                // ensure cancellation happens during real page execution.
                for (const auto& name :
                     QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                    if (!QFileInfo(QStringLiteral("/proc/self/fd/") + name)
                             .symLinkTarget()
                             .contains(QStringLiteral("memfd:mupdf-render-cancel")))
                        continue;
                    cookie = Mu::Worker::Sys::Mapping(
                        ::mmap(
                            nullptr, Mu::IPC::RenderCookieBytes, PROT_READ | PROT_WRITE, MAP_SHARED, name.toInt(), 0),
                        Mu::IPC::RenderCookieBytes);
                    break;
                }
            }
            if (cookie) {
                // Once queued, wait for the renderer here rather than relying
                // on the 10 ms client poll to catch a short replay window.
                while (std::chrono::steady_clock::now() - started < std::chrono::seconds(3)) {
                    if (static_cast<volatile fz_cookie*>(cookie.data())->progress > 0) {
                        sawProgress = true;
                        return true;
                    }
                    std::this_thread::yield();
                }
            }
            return std::chrono::steady_clock::now() - started > std::chrono::seconds(3);
        };
        const QRect tile = tiled ? QRect(0, 0, 1024, 1024) : QRect { };
        QVERIFY(m_client.render(0, 2048, 2048, tile, shouldAbort).isNull());
        QVERIFY(sawProgress);
        auto drained = std::async(std::launch::async, [&] { return m_client.isConnected(); });
        QVERIFY(drained.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QVERIFY(drained.get());

        // A late write to the old cookie must never abort the next render.
        const QImage next = m_client.render(0, 32, 32, { }, [&] {
            *static_cast<volatile std::int32_t*>(cookie.data()) = 1;
            return false;
        });
        QVERIFY(!next.isNull());
        QCOMPARE(next, expected);
        // The interrupted render published no frame and required no lease release.
        QCOMPARE(frameRequestId(next), frameRequestId(expected) + 2);
        QVERIFY(m_client.close());
    }

    void cancelledRendersDrainSafely()
    {
        QFETCH(bool, epub);
        QFETCH(int, finish);
        using namespace Mu::Model;
        QVERIFY(m_client.start(QStringLiteral(RENDER_WORKER_BUILD_PATH)));
        QList<::Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(m_client.open(epub ? m_epub : m_pdf, { }, pages, epub ? DocumentType::Epub : DocumentType::Pdf),
                 OpenStatus::Success);
        const QImage expected = m_client.render(0, 160, 160);
        QVERIFY(!expected.isNull());
        // Exercise cancellation alongside an existing retained frame lease.
        pid_t workerPid = -1;
        const int socket = workerControlSocket(workerPid);
        QVERIFY(socket >= 0);
        QCOMPARE(::kill(workerPid, SIGSTOP), 0);
        auto resume = qScopeGuard([&] { ::kill(workerPid, SIGCONT); });
        int status = 0;
        QCOMPARE(::waitpid(workerPid, &status, WUNTRACED), workerPid);
        QVERIFY(WIFSTOPPED(status));

        std::atomic<bool> cancel { false };
        auto active = std::async(std::launch::async,
                                 [&] { return m_client.render(0, 160, 160, { }, [&] { return cancel.load(); }); });
        // Ensure failed assertions also release the requesting thread before
        // the future's destructor joins it.
        const auto cancelOnExit = qScopeGuard([&] { cancel.store(true); });
        int pending = 0;
        const auto requestSent = [&] {
            return ::ioctl(socket, TIOCOUTQ, &pending) == 0 && pending > 0;
        };
        QTRY_VERIFY_WITH_TIMEOUT(requestSent(), 2000);
        QVERIFY(active.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        // The worker cannot answer yet, so this job must stay behind the active
        // RPC. Cancellation must return without waiting for either response.
        int checks = 0;
        QVERIFY(m_client.render(0, 160, 160, { }, [&] { return ++checks >= 3; }).isNull());
        cancel.store(true);
        QVERIFY(active.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QVERIFY(active.get().isNull());

        if (finish == 2) {
            QCOMPARE(::kill(workerPid, SIGKILL), 0);
            resume.dismiss();
            QVERIFY(m_client.render(0, 160, 160, { }, [] { return false; }).isNull());
            return;
        }
        QCOMPARE(::kill(workerPid, SIGCONT), 0);
        resume.dismiss();
        if (finish == 1) {
            m_client.stop();
            QCOMPARE(m_client.state(), ::Mu::Plugin::WorkerClient::State::Stopped);
            return;
        }
        // These barriers drain the cancelled RPC. It must publish no frame or
        // lease, and the queued cancelled job must consume no RPC request id.
        QVERIFY(m_client.isConnected());
        QVERIFY(m_client.isConnected());
        {
            const QImage next = m_client.render(0, 160, 160, { }, [] { return false; });
            QVERIFY(!next.isNull());
            // One cancelled render and the next render; no frame lease release.
            QCOMPARE(frameRequestId(next), frameRequestId(expected) + 2);
            QCOMPARE(next, expected);
        }
        // This also waits for the old response and its FD to be drained. Repeat
        // cancellation/completion races beyond the pool size to catch lease loss.
        for (int attempt = 0; attempt < 12; ++attempt) {
            checks = 0;
            QVERIFY(m_client.render(0, 160, 160, { }, [&] { return ++checks >= 2; }).isNull());
            const QImage image = m_client.render(0, 160, 160, { }, [] { return false; });
            QVERIFY(!image.isNull());
            QCOMPARE(image, expected);
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
