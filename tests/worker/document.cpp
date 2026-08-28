#include "engine/pdf/document.hpp"
#include "genpdf.hpp"
#include "runtime/command_service.hpp"
#include "shared/model/types.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void declareAcroForm(fz_context* context, pdf_document* document)
{
    pdf_obj* catalog = pdf_dict_get(context, pdf_trailer(context, document), PDF_NAME(Root));
    pdf_obj* acroForm = pdf_new_dict(context, document, 1);
    pdf_obj* fields = pdf_new_array(context, document, 0);
    pdf_dict_put_drop(context, acroForm, PDF_NAME(Fields), fields);
    pdf_dict_put_drop(context, catalog, PDF_NAME(AcroForm), acroForm);
}

std::vector<std::uint8_t>
renderPdfPage(const ::Mu::Worker::Engine::PdfDocument& doc, int page, int width, int height, std::string* error)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    if (!doc.renderToBuffer(
            { page, width, height, std::nullopt }, pixels.data(), static_cast<std::size_t>(width) * 4, error))
        return { };
    return pixels;
}

} // namespace

class TestDocument : public QObject {
    Q_OBJECT
private slots:

    void serviceStartsWithClosedDocument()
    {
        ::Mu::Worker::Runtime::CommandService service({ });
        const auto response = service.dispatch({ 1, ::Mu::Model::TextBoxesRequest { 0, 72, 72, true } });
        QVERIFY(response.error);
        QCOMPARE(response.error->code, ::Mu::Model::ErrorCode::NotOpen);
    }

    void testSavePdf()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("source.pdf");
        const QString pdfPath = directory.filePath("output.pdf");
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, source);
        fz_drop_context(context);

        QFile sourceFile(source);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(sourceFile.handle()), "source.pdf", &error), error.c_str());
        sourceFile.close();

        const int pdfFd = ::open(pdfPath.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(pdfFd >= 0);
        QVERIFY2(document.savePdfFd(pdfFd, { 0 }, &error), error.c_str());

        QFile pdfFile(pdfPath);
        QVERIFY(pdfFile.open(QIODevice::ReadOnly));
        const QByteArray content = pdfFile.read(32);
        pdfFile.close();
        QVERIFY2(content.startsWith("%PDF-"), content.constData());
    }

    void highlightAddRendersAndRoundTrips()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("source.pdf");
        const QString saved = directory.filePath("saved.pdf");
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, source);
        fz_drop_context(context);

        QFile sourceFile(source);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(sourceFile.handle()), "source.pdf", &error), error.c_str());
        sourceFile.close();

        const auto before = renderPdfPage(document, 0, 612, 792, &error);
        QVERIFY2(!before.empty(), error.c_str());
        ::Mu::Model::Annotation annotation;
        annotation.subtype = PDF_ANNOT_HIGHLIGHT;
        annotation.uuid = "highlight-round-trip";
        annotation.x0 = .1;
        annotation.y0 = .3;
        annotation.x1 = .4;
        annotation.y1 = .35;
        annotation.color = 0xffffff00U;
        annotation.flags = PDF_ANNOT_IS_PRINT;
        annotation.extras.quads.push_back({ { .1, .3 }, { .4, .3 }, { .4, .35 }, { .1, .35 } });
        std::int32_t object = -1;
        QVERIFY2(document.addAnnotation(0, annotation, &object, &error), error.c_str());
        QVERIFY(object > 0);

        const auto after = renderPdfPage(document, 0, 612, 792, &error);
        QVERIFY2(!after.empty(), error.c_str());
        QVERIFY(before != after);
        const auto annotations = document.extractAnnotations(0, &error);
        QVERIFY2(!annotations.empty(), error.c_str());
        QCOMPARE(annotations.back().extras.quads.size(), size_t(1));

        QFile savedFile(saved);
        QVERIFY(savedFile.open(QIODevice::WriteOnly));
        QVERIFY2(document.saveFd(savedFile.handle(), &error), error.c_str());
        savedFile.close();
        document.close();

        QFile savedInput(saved);
        QVERIFY(savedInput.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reloaded;
        QVERIFY2(reloaded.openFd(::dup(savedInput.handle()), "saved.pdf", &error), error.c_str());
        const auto roundTripped = reloaded.extractAnnotations(0, &error);
        QVERIFY2(!roundTripped.empty(), error.c_str());
        QCOMPARE(roundTripped.back().uuid, std::string("highlight-round-trip"));
        QCOMPARE(roundTripped.back().extras.quads.size(), size_t(1));
        QCOMPARE(roundTripped.back().color, annotation.color);
        const auto reloadedPixels = renderPdfPage(reloaded, 0, 612, 792, &error);
        QVERIFY2(!reloadedPixels.empty(), error.c_str());
        QVERIFY(before != reloadedPixels);
    }

    void annotationStressRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("source.pdf");
        const QString saved = directory.filePath("saved.pdf");
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, source);
        fz_drop_context(context);

        QFile sourceFile(source);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(sourceFile.handle()), "source.pdf", &error), error.c_str());
        const auto before = renderPdfPage(document, 0, 612, 792, &error);
        QVERIFY2(!before.empty(), error.c_str());

        constexpr int annotationCount = 120;
        std::vector<std::int32_t> objects;
        objects.reserve(annotationCount);
        for (int i = 0; i < annotationCount; ++i) {
            const double x0 = .03 + .095 * (i % 10);
            const double y0 = .03 + .08 * (i / 10);
            ::Mu::Model::Annotation annotation;
            annotation.subtype = PDF_ANNOT_HIGHLIGHT;
            annotation.uuid = "stress-" + std::to_string(i);
            annotation.x0 = x0;
            annotation.y0 = y0;
            annotation.x1 = x0 + .07;
            annotation.y1 = y0 + .03;
            annotation.color = i % 2 ? 0xffffff00U : 0xff00ff00U;
            annotation.flags = PDF_ANNOT_IS_PRINT;
            annotation.extras.quads.push_back(
                { { x0, y0 }, { annotation.x1, y0 }, { annotation.x1, annotation.y1 }, { x0, annotation.y1 } });
            std::int32_t object = -1;
            QVERIFY2(document.addAnnotation(0, annotation, &object, &error), error.c_str());
            QVERIFY(object > 0);
            objects.push_back(object);
            if (i % 3 == 0) {
                annotation.color = 0xffff00ffU;
                QVERIFY2(document.modifyAnnotation(0, object, annotation, true, &error), error.c_str());
            }
        }
        for (int i = 0; i < annotationCount; i += 5)
            QVERIFY2(document.removeAnnotation(0, objects.at(static_cast<std::size_t>(i)), &error), error.c_str());

        const auto after = renderPdfPage(document, 0, 612, 792, &error);
        QVERIFY2(!after.empty(), error.c_str());
        QVERIFY(before != after);
        constexpr int expectedCount = annotationCount - (annotationCount + 4) / 5;
        QCOMPARE(document.extractAnnotations(0, &error).size(), size_t(expectedCount));

        QFile savedFile(saved);
        QVERIFY(savedFile.open(QIODevice::WriteOnly));
        QVERIFY2(document.saveFd(savedFile.handle(), &error), error.c_str());
        savedFile.close();
        document.close();

        QFile savedInput(saved);
        QVERIFY(savedInput.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reloaded;
        QVERIFY2(reloaded.openFd(::dup(savedInput.handle()), "saved.pdf", &error), error.c_str());
        const auto annotations = reloaded.extractAnnotations(0, &error);
        QCOMPARE(annotations.size(), size_t(expectedCount));
        std::set<std::string> ids;
        for (const auto& annotation : annotations) {
            QVERIFY(annotation.uuid.starts_with("stress-"));
            QCOMPARE(annotation.extras.quads.size(), size_t(1));
            ids.insert(annotation.uuid);
        }
        QCOMPARE(ids.size(), size_t(expectedCount));
        const auto reloadedPixels = renderPdfPage(reloaded, 0, 612, 792, &error);
        QVERIFY2(!reloadedPixels.empty(), error.c_str());
        QVERIFY(before != reloadedPixels);
    }

    void failedDocumentOpenClearsCredentials()
    {
        ::Mu::Worker::Runtime::CommandService service({ });
        std::string error;
        // 1. Open invalid FD should return error and ensure no credentials stored
        const auto response = service.openFdResponse(1, -1, "invalid.pdf", "secret123", ::Mu::Model::DocumentType::Pdf);
        QVERIFY(response.error);
        QCOMPARE(response.error->code, ::Mu::Model::ErrorCode::InvalidRequest);

        // 2. Direct openFd with invalid FD fails and clears password
        QVERIFY(!service.openFd(-1, "invalid.pdf", ::Mu::Model::DocumentType::Pdf, &error));
    }

    void unknownDocumentTypeIsRejectedAndConsumesFd()
    {
        ::Mu::Worker::Runtime::CommandService service({ });
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        std::string error;
        QVERIFY(!service.openFd(fd, "unknown", ::Mu::Model::DocumentType::Unknown, &error));
        QVERIFY(error.find("unknown") != std::string::npos);
        QVERIFY(::fcntl(fd, F_GETFD) < 0);
        QCOMPARE(errno, EBADF);
    }

    void annotationGeometryIsClampedBeforeMuPdf()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("clamped.pdf"));
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, path);
        fz_drop_context(context);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Runtime::CommandService service({ });
        const auto opened = service.openFdResponse(1, ::dup(file.handle()), "clamped.pdf");
        QVERIFY2(!opened.error, opened.error ? opened.error->message.c_str() : "");

        ::Mu::Model::Annotation annotation;
        annotation.subtype = PDF_ANNOT_HIGHLIGHT;
        annotation.x0 = -10.0;
        annotation.y0 = 2.0;
        annotation.x1 = 10.0;
        annotation.y1 = -2.0;
        annotation.extras.quads.push_back({ { -1.0, 2.0 }, { 2.0, 2.0 }, { 2.0, -1.0 }, { -1.0, -1.0 } });
        const auto response = service.dispatch({ 2, ::Mu::Model::AnnotationAddRequest { 0, annotation } });
        QVERIFY2(!response.error, response.error ? response.error->message.c_str() : "");

        const auto handle = std::get<::Mu::Model::AnnotationResponse>(response.payload).handle;
        auto unsafe = annotation;
        unsafe.contents = std::string("unsafe\0suffix", 13);
        const auto rejectedAdd = service.dispatch({ 3, ::Mu::Model::AnnotationAddRequest { 0, unsafe } });
        QVERIFY(rejectedAdd.error);
        QCOMPARE(rejectedAdd.error->code, ::Mu::Model::ErrorCode::InvalidRequest);

        const auto rejectedModify =
            service.dispatch({ 4, ::Mu::Model::AnnotationModifyRequest { { 0, handle, unsafe, false } } });
        QVERIFY(rejectedModify.error);
        QCOMPARE(rejectedModify.error->code, ::Mu::Model::ErrorCode::InvalidRequest);

        std::string error;
        const auto annotations = service.document()->extractAnnotations(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(!annotations.empty());
        const auto& rect = annotations.back();
        QVERIFY(std::isfinite(rect.x0) && std::isfinite(rect.y0) && std::isfinite(rect.x1) && std::isfinite(rect.y1));
    }

    void malformedOpenCanRecoverOnSameDocument()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString malformedPath = directory.filePath("malformed.pdf");
        QFile malformed(malformedPath);
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        QVERIFY(malformed.write("%PDF-1.7\ntruncated", 18) == 18);
        malformed.close();

        const QString validPath = directory.filePath("valid.pdf");
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, validPath);
        fz_drop_context(context);

        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QFile malformedInput(malformedPath);
        QVERIFY(malformedInput.open(QIODevice::ReadOnly));
        const int malformedFd = ::dup(malformedInput.handle());
        QVERIFY(malformedFd >= 0);
        QVERIFY(!document.openFd(malformedFd, "malformed.pdf", &error));
        QVERIFY(!error.empty());
        QVERIFY(!document.isOpen());

        QFile validInput(validPath);
        QVERIFY(validInput.open(QIODevice::ReadOnly));
        error.clear();
        QVERIFY2(document.openFd(::dup(validInput.handle()), "valid.pdf", &error), error.c_str());
        QCOMPARE(document.pageCount(), 1);
        QVERIFY(document.pageGeometry(0, &error).widthPoints > 0.0);
    }

    void throwingSigningCallbackIsContainedAndFieldRemainsUsable()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString sourcePath = directory.filePath("source.pdf");
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createSignaturePDF(context, sourcePath);
        fz_drop_context(context);

        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(source.handle()), "source.pdf", &error), error.c_str());

        const auto fields = document.pageDetails(0, &error).signatures;
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(fields.size(), size_t(1));
        QVERIFY(!fields.front().signedField);

        const QString outputPath = directory.filePath("output.pdf");
        const int outputFd = ::open(outputPath.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outputFd >= 0);
        Mu::Model::SigningResult signingResult = Mu::Model::SigningResult::Success;
        QVERIFY(!document.signFd(
            { .file = { },
              .page = 0,
              .rectangle = { },
              .certificateNickname = "test-certificate",
              .certificateSubjectCommonName = "Test Signer",
              .reason = { },
              .location = { },
              .existingFieldObjectNumber = fields.front().objectNumber,
              .backgroundImage = { } },
            [](const std::array<std::uint8_t, 32>&, const std::string&) -> ::Mu::Worker::Engine::CmsResult {
                throw std::runtime_error("test callback failure");
            },
            outputFd,
            &signingResult,
            &error));
        QCOMPARE(signingResult, Mu::Model::SigningResult::GenericError);
        QVERIFY(!error.empty());
        QVERIFY(::fcntl(outputFd, F_GETFD) == -1);

        error.clear();
        const auto afterFailure = document.pageDetails(0, &error).signatures;
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(afterFailure.size(), size_t(1));
        QVERIFY(!afterFailure.front().signedField);
    }

    void deferredQueueRejectsOverflow()
    {
        ::Mu::Worker::Runtime::CommandService service({ });
        std::vector<std::byte> payload(1024, std::byte { 0x42 });

        for (std::size_t i = 0; i < ::Mu::Worker::Runtime::MaxDeferredFrames; ++i) {
            QVERIFY(service.deferIncoming(payload));
        }

        // Exceeding MaxDeferredFrames must reject with false
        QVERIFY(!service.deferIncoming(payload));

        // Taking frames restores capacity
        auto taken = service.takeDeferredIncoming();
        QVERIFY(taken.has_value());
        QCOMPARE(taken->size(), payload.size());
        QVERIFY(service.deferIncoming(payload));
    }

    void deferredQueueIsFifoAndByteBounded()
    {
        ::Mu::Worker::Runtime::CommandService service({ });
        const std::vector<std::byte> first(3, std::byte { 0x01 });
        const std::vector<std::byte> second(5, std::byte { 0x02 });
        QVERIFY(service.deferIncoming(first));
        QVERIFY(service.deferIncoming(second));

        const auto firstTaken = service.takeDeferredIncoming();
        QVERIFY(firstTaken.has_value());
        QCOMPARE(*firstTaken, first);
        const auto secondTaken = service.takeDeferredIncoming();
        QVERIFY(secondTaken.has_value());
        QCOMPARE(*secondTaken, second);
        QVERIFY(!service.takeDeferredIncoming().has_value());

        std::vector<std::byte> oversized(::Mu::Worker::Runtime::MaxDeferredBytes + 1, std::byte { 0x03 });
        QVERIFY(!service.deferIncoming(std::move(oversized)));
    }

    void extractFormFieldsHandlesCrossPageRadioGroupAndSelection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("radios_crosspage.pdf"));

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            declareAcroForm(ctx, pdfDoc);
            fz_buffer* contents1 = fz_new_buffer(ctx, 10);
            fz_buffer* contents2 = fz_new_buffer(ctx, 10);
            pdf_obj* res1 = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* res2 = pdf_new_dict(ctx, pdfDoc, 0);

            pdf_obj* pageObj1 = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, res1, contents1);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj1);
            pdf_obj* pageObj2 = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, res2, contents2);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj2);

            pdf_page* page1 = pdf_load_page(ctx, pdfDoc, 0);
            pdf_page* page2 = pdf_load_page(ctx, pdfDoc, 1);

            // Parent radio field
            pdf_obj* parentField = pdf_new_dict(ctx, pdfDoc, 4);
            pdf_dict_put(ctx, parentField, PDF_NAME(FT), PDF_NAME(Btn));
            pdf_dict_put_int(ctx, parentField, PDF_NAME(Ff), PDF_BTN_FIELD_IS_RADIO);
            pdf_dict_put_text_string(ctx, parentField, PDF_NAME(T), "RadioGroup");
            pdf_dict_put_name(ctx, parentField, PDF_NAME(V), "ChoiceA");

            // Radio on page 1 (checked)
            pdf_annot* w1 = pdf_create_annot(ctx, page1, PDF_ANNOT_WIDGET);
            pdf_obj* o1 = pdf_annot_obj(ctx, w1);
            pdf_dict_put(ctx, o1, PDF_NAME(Parent), parentField);
            pdf_dict_put_name(ctx, o1, PDF_NAME(AS), "ChoiceA");
            pdf_obj* ap1 = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_obj* n1 = pdf_new_dict(ctx, pdfDoc, 2);
            pdf_dict_puts_drop(ctx, n1, "Off", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_puts_drop(ctx, n1, "ChoiceA", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_put_drop(ctx, ap1, PDF_NAME(N), n1);
            pdf_dict_put_drop(ctx, o1, PDF_NAME(AP), ap1);

            // Radio on page 2 (unchecked)
            pdf_annot* w2 = pdf_create_annot(ctx, page2, PDF_ANNOT_WIDGET);
            pdf_obj* o2 = pdf_annot_obj(ctx, w2);
            pdf_dict_put(ctx, o2, PDF_NAME(Parent), parentField);
            pdf_dict_put_name(ctx, o2, PDF_NAME(AS), "Off");
            pdf_obj* ap2 = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_obj* n2 = pdf_new_dict(ctx, pdfDoc, 2);
            pdf_dict_puts_drop(ctx, n2, "Off", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_puts_drop(ctx, n2, "ChoiceB", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_put_drop(ctx, ap2, PDF_NAME(N), n2);
            pdf_dict_put_drop(ctx, o2, PDF_NAME(AP), ap2);

            pdf_update_page(ctx, page1);
            pdf_update_page(ctx, page2);
            pdf_drop_annot(ctx, w1);
            pdf_drop_annot(ctx, w2);
            pdf_drop_page(ctx, page1);
            pdf_drop_page(ctx, page2);
            pdf_drop_obj(ctx, parentField);
            pdf_drop_obj(ctx, pageObj1);
            pdf_drop_obj(ctx, pageObj2);
            pdf_drop_obj(ctx, res1);
            pdf_drop_obj(ctx, res2);
            fz_drop_buffer(ctx, contents1);
            fz_drop_buffer(ctx, contents2);

            pdf_save_document(ctx, pdfDoc, QFile::encodeName(path).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
        }
        fz_drop_context(ctx);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY(doc.openFd(::dup(file.handle()), "radios_crosspage.pdf", &error));

        const auto page0Details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(page0Details.formFields.size(), 1u);
        const auto& f0 = page0Details.formFields[0];
        QCOMPARE(f0.type, ::Mu::Model::FormFieldType::RadioButton);
        QCOMPARE(f0.groupName, std::string("RadioGroup"));
        QCOMPARE(f0.onState, std::string("ChoiceA"));
        QVERIFY(f0.checked);

        const auto page1Details = doc.pageDetails(1, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(page1Details.formFields.size(), 1u);
        const auto& f1 = page1Details.formFields[0];
        QCOMPARE(f1.type, ::Mu::Model::FormFieldType::RadioButton);
        QCOMPARE(f1.groupName, std::string("RadioGroup"));
        QCOMPARE(f1.fieldObjectNumber, f0.fieldObjectNumber);
        QCOMPARE(f1.onState, std::string("ChoiceB"));
        QVERIFY(!f1.checked);
    }

    void extractFormFieldsRejectsOversizedFieldString()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("oversized_form.pdf"));

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            declareAcroForm(ctx, pdfDoc);
            fz_buffer* contents = fz_new_buffer(ctx, 10);
            pdf_obj* resources = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, resources, contents);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_page* page = pdf_load_page(ctx, pdfDoc, 0);

            pdf_annot* widget = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
            pdf_obj* widgetObj = pdf_annot_obj(ctx, widget);
            pdf_dict_put(ctx, widgetObj, PDF_NAME(FT), PDF_NAME(Tx));

            // Create a field name string larger than MaxFormNameBytes (1024)
            std::string hugeName(2000, 'X');
            pdf_dict_put_text_string(ctx, widgetObj, PDF_NAME(T), hugeName.c_str());

            pdf_update_page(ctx, page);
            pdf_drop_annot(ctx, widget);
            pdf_drop_page(ctx, page);
            pdf_drop_obj(ctx, pageObj);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);

            pdf_save_document(ctx, pdfDoc, QFile::encodeName(path).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
        }
        fz_drop_context(ctx);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY(doc.openFd(::dup(file.handle()), "oversized_form.pdf", &error));
        const auto details = doc.pageDetails(0, &error);
        QVERIFY(!error.empty());
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("resource limit")));
    }

    void updateTextWithUnicodeCharacterLimit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("text_form.pdf"));

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            declareAcroForm(ctx, pdfDoc);
            fz_buffer* contents = fz_new_buffer(ctx, 10);
            pdf_obj* resources = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, resources, contents);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_page* page = pdf_load_page(ctx, pdfDoc, 0);

            pdf_annot* widget = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
            pdf_obj* widgetObj = pdf_annot_obj(ctx, widget);
            pdf_dict_put(ctx, widgetObj, PDF_NAME(FT), PDF_NAME(Tx));
            pdf_dict_put_text_string(ctx, widgetObj, PDF_NAME(T), "NameField");
            // Set max len to 5 characters (codepoints)
            pdf_dict_put_int(ctx, widgetObj, PDF_NAME(MaxLen), 5);

            pdf_update_page(ctx, page);
            pdf_drop_annot(ctx, widget);
            pdf_drop_page(ctx, page);
            pdf_drop_obj(ctx, pageObj);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);

            pdf_save_document(ctx, pdfDoc, QFile::encodeName(path).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
        }
        fz_drop_context(ctx);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY(doc.openFd(::dup(file.handle()), "text_form.pdf", &error));

        const auto details = doc.pageDetails(0, &error);
        QVERIFY(!details.formFields.empty());
        const auto& field = details.formFields[0];
        QCOMPARE(field.maximumLength, 5);

        // 5 UTF-8 multibyte characters (15 bytes total) must succeed
        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        const std::string validUtf8 = "你好世界！"; // 5 characters (3 bytes each)
        QVERIFY(doc.updateFormField(
            0, field.pdfObjectNumber, ::Mu::Model::FormTextValue { validUtf8 }, &mutations, &error));
        const auto mutation = std::find_if(mutations.begin(), mutations.end(), [&](const auto& item) {
            return item.objectNumber == field.pdfObjectNumber;
        });
        QVERIFY(mutation != mutations.end());
        const auto* resVal = std::get_if<::Mu::Model::FormTextValue>(&mutation->actualValue);
        QVERIFY(resVal != nullptr);
        QCOMPARE(resVal->text, validUtf8);

        // 6 characters (18 bytes) must be rejected
        mutations.clear();
        const std::string invalidUtf8 = "你好世界！！"; // 6 characters
        QVERIFY(!doc.updateFormField(
            0, field.pdfObjectNumber, ::Mu::Model::FormTextValue { invalidUtf8 }, &mutations, &error));
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("maximum length")));
    }

    void updateMultiselectAndEditableComboPersistAcrossReopen()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sourcePath = dir.filePath(QStringLiteral("choices.pdf"));
        const QString savedPath = dir.filePath(QStringLiteral("choices-saved.pdf"));

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        QVERIFY(ctx);
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            declareAcroForm(ctx, pdfDoc);
            pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
            pdf_obj* fields = pdf_dict_get(ctx, pdf_dict_get(ctx, catalog, PDF_NAME(AcroForm)), PDF_NAME(Fields));
            fz_buffer* contents = fz_new_buffer(ctx, 10);
            pdf_obj* resources = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, resources, contents);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_page* page = pdf_load_page(ctx, pdfDoc, 0);

            auto createChoiceWidget = [&](const char* name, int flags) {
                pdf_annot* widget = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
                pdf_obj* object = pdf_annot_obj(ctx, widget);
                pdf_dict_put(ctx, object, PDF_NAME(FT), PDF_NAME(Ch));
                pdf_dict_put_int(ctx, object, PDF_NAME(Ff), flags);
                pdf_dict_put_text_string(ctx, object, PDF_NAME(T), name);
                pdf_obj* options = pdf_new_array(ctx, pdfDoc, 3);
                for (const auto& option :
                     { std::pair { "One", "1" }, std::pair { "Two", "2" }, std::pair { "Three", "3" } }) {
                    pdf_obj* pair = pdf_new_array(ctx, pdfDoc, 2);
                    pdf_array_push_drop(ctx, pair, pdf_new_text_string(ctx, option.first));
                    pdf_array_push_drop(ctx, pair, pdf_new_text_string(ctx, option.second));
                    pdf_array_push_drop(ctx, options, pair);
                }
                pdf_dict_put_drop(ctx, object, PDF_NAME(Opt), options);
                pdf_array_push(ctx, fields, object);
                return widget;
            };

            pdf_annot* list = createChoiceWidget("MultiList", PDF_CH_FIELD_IS_MULTI_SELECT);
            pdf_annot* combo = createChoiceWidget("EditableCombo", PDF_CH_FIELD_IS_COMBO | PDF_CH_FIELD_IS_EDIT);
            pdf_update_page(ctx, page);
            pdf_drop_annot(ctx, list);
            pdf_drop_annot(ctx, combo);
            pdf_drop_page(ctx, page);
            pdf_drop_obj(ctx, pageObj);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);
            pdf_save_document(ctx, pdfDoc, QFile::encodeName(sourcePath).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
            QFAIL(fz_caught_message(ctx));
        }
        fz_drop_context(ctx);

        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(sourceFile.handle()), "choices.pdf", &error), error.c_str());
        const auto initial = document.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto listField = std::find_if(initial.formFields.begin(),
                                            initial.formFields.end(),
                                            [](const auto& field) { return field.partialName == "MultiList"; });
        const auto comboField = std::find_if(initial.formFields.begin(),
                                             initial.formFields.end(),
                                             [](const auto& field) { return field.partialName == "EditableCombo"; });
        QVERIFY(listField != initial.formFields.end());
        QVERIFY(comboField != initial.formFields.end());
        QVERIFY(listField->multiSelect);
        QVERIFY(comboField->editableCombo);

        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY(document.updateFormField(
            0, listField->pdfObjectNumber, ::Mu::Model::FormChoiceSelection { { 0, 2 } }, &mutations, &error));
        QVERIFY(document.updateFormField(
            0, comboField->pdfObjectNumber, ::Mu::Model::FormChoiceCustomText { "Custom" }, &mutations, &error));

        const int outputFd = ::open(savedPath.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outputFd >= 0);
        QVERIFY2(document.saveFd(outputFd, &error), error.c_str());

        QFile savedFile(savedPath);
        QVERIFY(savedFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reopened;
        QVERIFY2(reopened.openFd(::dup(savedFile.handle()), "choices-saved.pdf", &error), error.c_str());
        const auto persisted = reopened.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto persistedList = std::find_if(persisted.formFields.begin(),
                                                persisted.formFields.end(),
                                                [](const auto& field) { return field.partialName == "MultiList"; });
        const auto persistedCombo =
            std::find_if(persisted.formFields.begin(), persisted.formFields.end(), [](const auto& field) {
                return field.partialName == "EditableCombo";
            });
        QVERIFY(persistedList != persisted.formFields.end());
        QVERIFY(persistedCombo != persisted.formFields.end());
        QCOMPARE(persistedList->currentChoices, std::vector<int>({ 0, 2 }));
        QVERIFY(persistedCombo->currentChoices.empty());
        QCOMPARE(persistedCombo->text, std::string("Custom"));
    }

    void updateRadioGroupAtomicallySynchronizesSiblings()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("radios_sync.pdf"));

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            declareAcroForm(ctx, pdfDoc);
            fz_buffer* contents1 = fz_new_buffer(ctx, 10);
            fz_buffer* contents2 = fz_new_buffer(ctx, 10);
            pdf_obj* res1 = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* res2 = pdf_new_dict(ctx, pdfDoc, 0);

            pdf_obj* pageObj1 = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, res1, contents1);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj1);
            pdf_obj* pageObj2 = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, res2, contents2);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj2);

            pdf_page* page1 = pdf_load_page(ctx, pdfDoc, 0);
            pdf_page* page2 = pdf_load_page(ctx, pdfDoc, 1);

            // Parent radio field with Kids
            pdf_obj* parentField = pdf_new_dict(ctx, pdfDoc, 5);
            pdf_dict_put(ctx, parentField, PDF_NAME(FT), PDF_NAME(Btn));
            pdf_dict_put_int(
                ctx, parentField, PDF_NAME(Ff), PDF_BTN_FIELD_IS_RADIO | PDF_BTN_FIELD_IS_NO_TOGGLE_TO_OFF);
            pdf_dict_put_text_string(ctx, parentField, PDF_NAME(T), "RadioSync");
            pdf_dict_put_name(ctx, parentField, PDF_NAME(V), "ChoiceA");
            pdf_obj* containerField = pdf_new_dict(ctx, pdfDoc, 3);
            pdf_dict_put(ctx, containerField, PDF_NAME(FT), PDF_NAME(Btn));
            pdf_dict_put_text_string(ctx, containerField, PDF_NAME(T), "OuterContainer");
            pdf_dict_put_name(ctx, containerField, PDF_NAME(V), "ContainerValue");
            pdf_dict_put(ctx, parentField, PDF_NAME(Parent), containerField);

            // Radio on page 1 (initially ChoiceA / checked)
            pdf_annot* w1 = pdf_create_annot(ctx, page1, PDF_ANNOT_WIDGET);
            pdf_obj* o1 = pdf_annot_obj(ctx, w1);
            pdf_dict_put(ctx, o1, PDF_NAME(Parent), parentField);
            pdf_dict_put_name(ctx, o1, PDF_NAME(AS), "ChoiceA");
            pdf_obj* ap1 = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_obj* n1 = pdf_new_dict(ctx, pdfDoc, 2);
            pdf_dict_puts_drop(ctx, n1, "Off", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_puts_drop(ctx, n1, "ChoiceA", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_put_drop(ctx, ap1, PDF_NAME(N), n1);
            pdf_dict_put_drop(ctx, o1, PDF_NAME(AP), ap1);

            // Radio on page 2 (initially Off / unchecked)
            pdf_annot* w2 = pdf_create_annot(ctx, page2, PDF_ANNOT_WIDGET);
            pdf_obj* o2 = pdf_annot_obj(ctx, w2);
            pdf_dict_put(ctx, o2, PDF_NAME(Parent), parentField);
            pdf_dict_put_name(ctx, o2, PDF_NAME(AS), "Off");
            pdf_obj* ap2 = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_obj* n2 = pdf_new_dict(ctx, pdfDoc, 2);
            pdf_dict_puts_drop(ctx, n2, "Off", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_puts_drop(ctx, n2, "ChoiceB", pdf_new_dict(ctx, pdfDoc, 0));
            pdf_dict_put_drop(ctx, ap2, PDF_NAME(N), n2);
            pdf_dict_put_drop(ctx, o2, PDF_NAME(AP), ap2);

            pdf_obj* kids = pdf_new_array(ctx, pdfDoc, 2);
            pdf_array_push(ctx, kids, o1);
            pdf_array_push(ctx, kids, o2);
            pdf_dict_put_drop(ctx, parentField, PDF_NAME(Kids), kids);

            pdf_update_page(ctx, page1);
            pdf_update_page(ctx, page2);
            pdf_drop_annot(ctx, w1);
            pdf_drop_annot(ctx, w2);
            pdf_drop_page(ctx, page1);
            pdf_drop_page(ctx, page2);
            pdf_drop_obj(ctx, parentField);
            pdf_drop_obj(ctx, containerField);
            pdf_drop_obj(ctx, pageObj1);
            pdf_drop_obj(ctx, pageObj2);
            pdf_drop_obj(ctx, res1);
            pdf_drop_obj(ctx, res2);
            fz_drop_buffer(ctx, contents1);
            fz_drop_buffer(ctx, contents2);

            pdf_save_document(ctx, pdfDoc, QFile::encodeName(path).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
        }
        fz_drop_context(ctx);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY(doc.openFd(::dup(file.handle()), "radios_sync.pdf", &error));

        const auto p0 = doc.pageDetails(0, &error);
        const auto p1 = doc.pageDetails(1, &error);
        QCOMPARE(p0.formFields.size(), 1u);
        QCOMPARE(p1.formFields.size(), 1u);
        QVERIFY(p0.formFields[0].checked);
        QVERIFY(!p1.formFields[0].checked);

        // Selecting Radio on Page 1 (ChoiceB) must atomically uncheck Radio on Page 0 (ChoiceA)
        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY(doc.updateFormField(
            1, p1.formFields[0].pdfObjectNumber, ::Mu::Model::FormCheckValue { true }, &mutations, &error));
        QCOMPARE(mutations.size(), 2u);

        // One is checked (page 1, ChoiceB), sibling is unchecked (page 0, ChoiceA)
        bool page1Checked = false;
        bool page0Unchecked = false;
        for (const auto& m : mutations) {
            const auto* c = std::get_if<::Mu::Model::FormCheckValue>(&m.actualValue);
            QVERIFY(c != nullptr);
            if (m.page == 1 && c->checked)
                page1Checked = true;
            if (m.page == 0 && !c->checked)
                page0Unchecked = true;
        }
        QVERIFY(page1Checked);
        QVERIFY(page0Unchecked);

        // Directly unchecking radio button in NoToggleToOff group must be rejected
        mutations.clear();
        QVERIFY(!doc.updateFormField(
            1, p1.formFields[0].pdfObjectNumber, ::Mu::Model::FormCheckValue { false }, &mutations, &error));
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("cannot be unchecked directly")));

        error.clear();
        const QString savedPath = dir.filePath(QStringLiteral("radios_sync-saved.pdf"));
        const int outputFd = ::open(savedPath.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(outputFd >= 0);
        QVERIFY2(doc.saveFd(outputFd, &error), error.c_str());

        QFile savedFile(savedPath);
        QVERIFY(savedFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reopened;
        QVERIFY2(reopened.openFd(::dup(savedFile.handle()), "radios_sync-saved.pdf", &error), error.c_str());
        const auto savedPage0 = reopened.pageDetails(0, &error);
        const auto savedPage1 = reopened.pageDetails(1, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(!savedPage0.formFields[0].checked);
        QVERIFY(savedPage1.formFields[0].checked);

        fz_context* checkContext = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        QVERIFY(checkContext);
        fz_register_document_handlers(checkContext);
        fz_document* checkDocument = fz_open_document(checkContext, QFile::encodeName(savedPath).constData());
        QVERIFY(checkDocument);
        pdf_document* checkPdf = pdf_specifics(checkContext, checkDocument);
        pdf_page* checkPage = pdf_load_page(checkContext, checkPdf, 1);
        pdf_annot* checkWidget = pdf_first_widget(checkContext, checkPage);
        QVERIFY(checkWidget);
        pdf_obj* checkField = pdf_annot_obj(checkContext, checkWidget);
        pdf_obj* logicalField = pdf_dict_get(checkContext, checkField, PDF_NAME(Parent));
        pdf_obj* outerField = pdf_dict_get(checkContext, logicalField, PDF_NAME(Parent));
        QCOMPARE(std::string(pdf_to_name(checkContext, pdf_dict_get(checkContext, logicalField, PDF_NAME(V)))),
                 std::string("ChoiceB"));
        QCOMPARE(std::string(pdf_to_name(checkContext, pdf_dict_get(checkContext, outerField, PDF_NAME(V)))),
                 std::string("ContainerValue"));
        pdf_drop_page(checkContext, checkPage);
        fz_drop_document(checkContext, checkDocument);
        fz_drop_context(checkContext);
    }

    void updateRejectsReadOnlyAndMismatchedVariants()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("readonly_form.pdf"));

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            declareAcroForm(ctx, pdfDoc);
            fz_buffer* contents = fz_new_buffer(ctx, 10);
            pdf_obj* resources = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, resources, contents);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_page* page = pdf_load_page(ctx, pdfDoc, 0);

            // Read-only text field
            pdf_annot* w1 = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
            pdf_obj* o1 = pdf_annot_obj(ctx, w1);
            pdf_dict_put(ctx, o1, PDF_NAME(FT), PDF_NAME(Tx));
            pdf_dict_put_text_string(ctx, o1, PDF_NAME(T), "ReadOnlyText");
            pdf_dict_put_int(ctx, o1, PDF_NAME(Ff), PDF_FIELD_IS_READ_ONLY);

            // Non-editable combobox
            pdf_annot* w2 = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
            pdf_obj* o2 = pdf_annot_obj(ctx, w2);
            pdf_dict_put(ctx, o2, PDF_NAME(FT), PDF_NAME(Ch));
            pdf_dict_put_int(ctx, o2, PDF_NAME(Ff), PDF_CH_FIELD_IS_COMBO); // not PDF_CH_FIELD_IS_EDIT
            pdf_dict_put_text_string(ctx, o2, PDF_NAME(T), "StaticCombo");
            pdf_obj* opt = pdf_new_array(ctx, pdfDoc, 2);
            pdf_array_push_drop(ctx, opt, pdf_new_text_string(ctx, "OptionA"));
            pdf_array_push_drop(ctx, opt, pdf_new_text_string(ctx, "OptionB"));
            pdf_dict_put_drop(ctx, o2, PDF_NAME(Opt), opt);

            pdf_update_page(ctx, page);
            pdf_drop_annot(ctx, w1);
            pdf_drop_annot(ctx, w2);
            pdf_drop_page(ctx, page);
            pdf_drop_obj(ctx, pageObj);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);

            pdf_save_document(ctx, pdfDoc, QFile::encodeName(path).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
        }
        fz_drop_context(ctx);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY(doc.openFd(::dup(file.handle()), "readonly_form.pdf", &error));

        const auto details = doc.pageDetails(0, &error);
        QCOMPARE(details.formFields.size(), 2u);

        const auto* roField = &details.formFields[0];
        const auto* comboField = &details.formFields[1];
        if (roField->type != ::Mu::Model::FormFieldType::Text)
            std::swap(roField, comboField);

        QVERIFY(roField->readOnly);
        QCOMPARE(roField->type, ::Mu::Model::FormFieldType::Text);
        QCOMPARE(comboField->type, ::Mu::Model::FormFieldType::ComboBox);
        QVERIFY(!comboField->editableCombo);

        // 1. Modifying read-only field must be rejected
        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY(!doc.updateFormField(
            0, roField->pdfObjectNumber, ::Mu::Model::FormTextValue { "NewText" }, &mutations, &error));
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("read-only")));

        // 2. Mismatched variant (bool into combobox field) must be rejected
        mutations.clear();
        QVERIFY(!doc.updateFormField(
            0, comboField->pdfObjectNumber, ::Mu::Model::FormCheckValue { true }, &mutations, &error));
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("not a checkbox")));

        // 3. Custom text on non-editable combobox must be rejected
        mutations.clear();
        QVERIFY(!doc.updateFormField(
            0, comboField->pdfObjectNumber, ::Mu::Model::FormChoiceCustomText { "CustomText" }, &mutations, &error));
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("not editable")));
    }

    void resetButtonRestoresDefaultValues()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("reset_form.pdf"));

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            declareAcroForm(ctx, pdfDoc);
            fz_buffer* contents = fz_new_buffer(ctx, 10);
            pdf_obj* resources = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, resources, contents);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_page* page = pdf_load_page(ctx, pdfDoc, 0);

            pdf_annot* textWidget = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
            pdf_obj* textObject = pdf_annot_obj(ctx, textWidget);
            pdf_dict_put(ctx, textObject, PDF_NAME(FT), PDF_NAME(Tx));
            pdf_dict_put_text_string(ctx, textObject, PDF_NAME(T), "Name");
            pdf_dict_put_text_string(ctx, textObject, PDF_NAME(V), "Changed");
            pdf_dict_put_text_string(ctx, textObject, PDF_NAME(DV), "Default");

            pdf_annot* inertWidget = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
            pdf_obj* inertObject = pdf_annot_obj(ctx, inertWidget);
            pdf_dict_put(ctx, inertObject, PDF_NAME(FT), PDF_NAME(Btn));
            pdf_dict_put_int(ctx, inertObject, PDF_NAME(Ff), PDF_BTN_FIELD_IS_PUSHBUTTON);
            pdf_dict_put_text_string(ctx, inertObject, PDF_NAME(T), "InertReset");
            pdf_obj* inertAppearance = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_dict_put_text_string(ctx, inertAppearance, PDF_NAME(CA), "Reset");
            pdf_dict_put_drop(ctx, inertObject, PDF_NAME(MK), inertAppearance);

            pdf_annot* resetWidget = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
            pdf_obj* resetObject = pdf_annot_obj(ctx, resetWidget);
            pdf_dict_put(ctx, resetObject, PDF_NAME(FT), PDF_NAME(Btn));
            pdf_dict_put_int(ctx, resetObject, PDF_NAME(Ff), PDF_BTN_FIELD_IS_PUSHBUTTON);
            pdf_dict_put_text_string(ctx, resetObject, PDF_NAME(T), "Reset");
            pdf_obj* action = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_dict_put(ctx, action, PDF_NAME(S), PDF_NAME(ResetForm));
            pdf_dict_put_drop(ctx, resetObject, PDF_NAME(A), action);
            pdf_obj* appearance = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_dict_put_text_string(ctx, appearance, PDF_NAME(CA), "Reset all");
            pdf_dict_put_drop(ctx, resetObject, PDF_NAME(MK), appearance);

            pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
            pdf_obj* acroForm = pdf_dict_get(ctx, catalog, PDF_NAME(AcroForm));
            pdf_obj* fields = pdf_dict_get(ctx, acroForm, PDF_NAME(Fields));
            pdf_array_push(ctx, fields, textObject);
            pdf_array_push(ctx, fields, inertObject);
            pdf_array_push(ctx, fields, resetObject);

            pdf_update_page(ctx, page);
            pdf_drop_annot(ctx, textWidget);
            pdf_drop_annot(ctx, inertWidget);
            pdf_drop_annot(ctx, resetWidget);
            pdf_drop_page(ctx, page);
            pdf_drop_obj(ctx, pageObj);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);
            pdf_save_document(ctx, pdfDoc, QFile::encodeName(path).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
        }
        fz_drop_context(ctx);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(file.handle()), "reset_form.pdf", &error), error.c_str());

        const auto details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.formFields.size(), 3u);
        const auto reset = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.type == ::Mu::Model::FormFieldType::PushButton
                && field.pushButtonAction == ::Mu::Model::FormPushButtonAction::Reset;
        });
        const auto inert = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.type == ::Mu::Model::FormFieldType::PushButton
                && field.pushButtonAction == ::Mu::Model::FormPushButtonAction::None;
        });
        const auto text = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.type == ::Mu::Model::FormFieldType::Text;
        });
        QVERIFY(reset != details.formFields.end());
        QVERIFY(inert != details.formFields.end());
        QVERIFY(text != details.formFields.end());
        QCOMPARE(reset->buttonCaption, std::string("Reset all"));
        QCOMPARE(reset->pushButtonAction, ::Mu::Model::FormPushButtonAction::Reset);
        QCOMPARE(inert->buttonCaption, std::string("Reset"));
        QCOMPARE(text->text, std::string("Changed"));

        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY(!doc.resetForm(0, inert->pdfObjectNumber, &mutations, &error));
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("does not reset")));
        error.clear();
        QVERIFY2(doc.resetForm(0, reset->pdfObjectNumber, &mutations, &error), error.c_str());
        const auto mutation = std::find_if(mutations.begin(), mutations.end(), [&](const auto& item) {
            return item.objectNumber == text->pdfObjectNumber;
        });
        QVERIFY(mutation != mutations.end());
        const auto* value = std::get_if<::Mu::Model::FormTextValue>(&mutation->actualValue);
        QVERIFY(value != nullptr);
        QCOMPARE(value->text, std::string("Default"));
    }
};

int runTestWorkerDocument(int argc, char** argv)
{
    TestDocument test;
    return QTest::qExec(&test, argc, argv);
}

#include "document.moc"
