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

public:
    enum Coverage { NoText, VisibleText, HiddenText, FullHiddenText, WhiteCover, BlackCover, PartialCover };
    Q_ENUM(Coverage)

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
        QString recognized;
        for (const Mu::Model::TextBox& box : result.boxes) {
            recognized += QString::fromStdString(box.text);
            QVERIFY(box.left >= 0.0 && box.left <= box.right && box.right <= 1.0);
            QVERIFY(box.top >= 0.0 && box.top <= box.bottom && box.bottom <= 1.0);
        }
        QVERIFY2(recognized.contains(QStringLiteral("Hello"), Qt::CaseInsensitive), qPrintable(recognized));
    }

    void testImageOnlyOcr_data()
    {
        QTest::addColumn<Coverage>("coverage");
        QTest::addColumn<int>("rotation");
        QTest::addColumn<bool>("clipped");
        QTest::addColumn<bool>("duplicate");
        QTest::addColumn<bool>("shiftedCrop");
        QTest::addColumn<QString>("expected");
        QTest::newRow("image-only") << NoText << 0 << false << false << false << QStringLiteral("COVEREDUNCOVERED");
        QTest::newRow("visible-partial") << VisibleText << 0 << false << false << false << QStringLiteral("UNCOVERED");
        QTest::newRow("hidden-partial") << HiddenText << 0 << false << false << false << QStringLiteral("UNCOVERED");
        QTest::newRow("hidden-complete") << FullHiddenText << 0 << false << false << false << QString();
        QTest::newRow("displayed-upright-rotated")
            << HiddenText << 90 << false << false << false << QStringLiteral("UNCOVERED");
        QTest::newRow("white-cover") << WhiteCover << 0 << false << false << false << QString();
        QTest::newRow("black-cover") << BlackCover << 0 << false << false << false << QString();
        QTest::newRow("partial-cover") << PartialCover << 0 << false << false << false << QStringLiteral("UNCOVERED");
        QTest::newRow("clipped") << HiddenText << 0 << true << false << false << QString();
        QTest::newRow("overlapping-images") << HiddenText << 0 << false << true << false << QStringLiteral("UNCOVERED");
        QTest::newRow("shifted-crop") << HiddenText << 0 << false << false << true << QStringLiteral("UNCOVERED");
    }

    void testImageOnlyOcr()
    {
        QFETCH(Coverage, coverage);
        QFETCH(int, rotation);
        QFETCH(bool, clipped);
        QFETCH(bool, duplicate);
        QFETCH(bool, shiftedCrop);
        QFETCH(QString, expected);
        if (!QFile::exists(QStringLiteral(TESSDATA_DIR "/eng.traineddata")))
            QSKIP("English traineddata is not installed");
        const QString sourcePath = m_tempDir.filePath("image-source.pdf");
        const QString path = m_tempDir.filePath("image-ocr.pdf");
        auto* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        fz_register_document_handlers(context);
        createTextPDF(context, sourcePath, "BT /F1 24 Tf 72 650 Td (COVERED) Tj 0 -60 Td (UNCOVERED) Tj ET");
        fz_document* volatile source = nullptr;
        fz_page* volatile sourcePage = nullptr;
        fz_pixmap* volatile pixmap = nullptr;
        fz_image* volatile image = nullptr;
        fz_font* volatile font = nullptr;
        pdf_document* volatile document = nullptr;
        pdf_page* volatile target = nullptr;
        pdf_obj* volatile resources = nullptr;
        pdf_obj* volatile page = nullptr;
        fz_buffer* volatile contents = nullptr;
        volatile bool created = false;
        fz_rect expectedRegion { };
        fz_rect displayBounds { };
        fz_var(expectedRegion);
        fz_var(displayBounds);
        QByteArray stream;
        fz_try(context)
        {
            source = fz_open_document(context, QFile::encodeName(sourcePath).constData());
            sourcePage = fz_load_page(context, source, 0);
            pixmap = fz_new_pixmap_from_page(context, sourcePage, fz_scale(2, 2), fz_device_rgb(context), 0);
            image = fz_new_image_from_pixmap(context, pixmap, nullptr);
            document = pdf_create_document(context);
            resources = pdf_new_dict(context, document, 2);
            auto* images = pdf_dict_put_dict(context, resources, PDF_NAME(XObject), 1);
            pdf_dict_puts_drop(context, images, "Im0", pdf_add_image(context, document, image));
            font = fz_new_base14_font(context, "Helvetica");
            auto* fonts = pdf_dict_put_dict(context, resources, PDF_NAME(Font), 1);
            pdf_dict_puts_drop(
                context, fonts, "F1", pdf_add_simple_font(context, document, font, PDF_SIMPLE_ENCODING_LATIN));
            // Counterrotate the content so /Rotate makes the displayed scan upright.
            const fz_matrix contentTransform = rotation == 90 ? fz_matrix { 0, 1, -1, 0, 792, 0 } : fz_identity;
            stream = rotation == 90 ? "q 0 1 -1 0 792 0 cm q " : "q ";
            if (clipped)
                stream += "0 630 612 100 re W n ";
            stream += "612 0 0 792 0 0 cm /Im0 Do Q\n";
            if (duplicate)
                stream += "q 612 0 0 792 0 0 cm /Im0 Do Q\n";
            stream += "BT /F1 24 Tf 72 450 Td (NATIVEONLY) Tj ET\n";
            if (coverage == VisibleText || coverage == HiddenText || coverage == FullHiddenText) {
                stream += coverage == VisibleText ? "BT 0 Tr " : "BT 3 Tr ";
                stream += "/F1 24 Tf 72 650 Td (COVERED) Tj ";
                if (coverage == FullHiddenText)
                    stream += "0 -60 Td (UNCOVERED) Tj ";
                stream += "ET\n";
            }
            if (coverage == WhiteCover || coverage == BlackCover || coverage == PartialCover) {
                stream += coverage == BlackCover ? "0 g " : "1 g ";
                stream += coverage == PartialCover ? "60 630 300 70 re f\n" : "60 560 300 150 re f\n";
            }
            if (rotation == 90)
                stream += "Q\n";
            contents = fz_new_buffer_from_copied_data(context,
                                                      reinterpret_cast<const unsigned char*>(stream.constData()),
                                                      static_cast<std::size_t>(stream.size()));
            const fz_rect mediaBox = rotation == 90 ? fz_rect { 0, 0, 792, 612 } : fz_rect { 0, 0, 612, 792 };
            page = pdf_add_page(context, document, mediaBox, rotation, resources, contents);
            if (shiftedCrop)
                pdf_dict_put_rect(context, page, PDF_NAME(CropBox), { 40, 400, 500, 750 });
            pdf_insert_page(context, document, -1, page);
            target = pdf_load_page(context, document, 0);
            fz_rect mediabox;
            fz_matrix transform;
            pdf_page_transform(context, target, &mediabox, &transform);
            expectedRegion = fz_transform_rect({ 60, 560, 300, coverage == NoText ? 700.0f : 640.0f },
                                               fz_concat(contentTransform, transform));
            displayBounds = fz_bound_page(context, &target->super);
            pdf_save_document(context, document, QFile::encodeName(path).constData(), &pdf_default_write_options);
            created = true;
        }
        fz_always(context)
        {
            fz_drop_buffer(context, contents);
            pdf_drop_obj(context, page);
            pdf_drop_obj(context, resources);
            pdf_drop_page(context, target);
            pdf_drop_document(context, document);
            fz_drop_font(context, font);
            fz_drop_image(context, image);
            fz_drop_pixmap(context, pixmap);
            fz_drop_page(context, sourcePage);
            fz_drop_document(context, source);
        }
        fz_catch(context)
        {
            qWarning() << fz_caught_message(context);
        }
        fz_drop_context(context);
        QVERIFY(created);
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY);
        QVERIFY(fd >= 0);
        const auto result = Mu::Worker::Engine::runOcr(fd, { }, 0, "eng", 225);
        QCOMPARE(result.status, Mu::Model::OcrStatus::Success);
        QString recognized;
        for (const auto& box : result.boxes) {
            recognized += QString::fromStdString(box.text);
            QVERIFY(box.left >= 0 && box.left <= box.right && box.right <= 1);
            QVERIFY(box.top >= 0 && box.top <= box.bottom && box.bottom <= 1);
            const fz_point center {
                static_cast<float>(displayBounds.x0
                                   + (box.left + box.right) / 2 * (displayBounds.x1 - displayBounds.x0)),
                static_cast<float>(displayBounds.y0
                                   + (box.top + box.bottom) / 2 * (displayBounds.y1 - displayBounds.y0))
            };
            QVERIFY(fz_is_point_inside_rect(center, expectedRegion));
        }
        recognized.remove(' ');
        QCOMPARE(recognized, expected);
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
