#include "engine/pdf/document.hpp"
#include "engine/constants.hpp"
#include "genpdf.hpp"
#include "runtime/command_service.hpp"
#include "shared/model/types.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void failPdfSeek(fz_context* context, fz_stream*, std::int64_t, int)
{
    fz_throw(context, FZ_ERROR_SYSTEM, "injected PDF read failure");
}

void declareAcroForm(fz_context* context, pdf_document* document)
{
    pdf_obj* catalog = pdf_dict_get(context, pdf_trailer(context, document), PDF_NAME(Root));
    pdf_obj* acroForm = pdf_new_dict(context, document, 1);
    pdf_obj* fields = pdf_new_array(context, document, 0);
    pdf_dict_put_drop(context, acroForm, PDF_NAME(Fields), fields);
    pdf_dict_put_drop(context, catalog, PDF_NAME(AcroForm), acroForm);
}

// Preserve fixture streams and malformed graphs without PDF-writer repair.
bool writePdfObjects(const QString& path, const QList<QByteArray>& objects)
{
    QByteArray data("%PDF-1.7\n");
    QList<qsizetype> offsets;
    for (qsizetype index = 0; index < objects.size(); ++index) {
        offsets.push_back(data.size());
        data += QByteArray::number(index + 1) + " 0 obj\n" + objects[index] + "\nendobj\n";
    }
    const qsizetype xref = data.size();
    data += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const auto offset : offsets)
        data += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    data += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n"
        + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
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

std::vector<std::uint8_t>
renderPdfUsage(const ::Mu::Worker::Engine::PdfDocument& document, int pageNumber, const char* usage, std::string* error)
{
    fz_context* context = document.context();
    pdf_page* volatile page = nullptr;
    fz_pixmap* volatile pixmap = nullptr;
    std::vector<std::uint8_t> pixels;
    fz_try(context)
    {
        page = pdf_load_page(context, pdf_specifics(context, document.document()), pageNumber);
        pixmap = pdf_new_pixmap_from_page_with_usage(
            context, page, fz_identity, fz_device_rgb(context), 0, usage, FZ_CROP_BOX);
        const auto size = static_cast<std::size_t>(fz_pixmap_stride(context, pixmap))
            * static_cast<std::size_t>(fz_pixmap_height(context, pixmap));
        const auto* samples = fz_pixmap_samples(context, pixmap);
        pixels.assign(samples, samples + size);
    }
    fz_always(context)
    {
        fz_drop_pixmap(context, pixmap);
        fz_drop_page(context, reinterpret_cast<fz_page*>(page));
    }
    fz_catch(context)
    {
        *error = fz_caught_message(context);
    }
    return pixels;
}

bool pixelsMatch(const std::vector<std::uint8_t>& actual, const std::vector<std::uint8_t>& expected)
{
    return !expected.empty() && actual.size() == expected.size()
        && std::equal(actual.begin(), actual.end(), expected.begin(), [](auto a, auto b) {
               return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 2;
           });
}

} // namespace

class TestDocument : public QObject {
    Q_OBJECT
private slots:

    void attachmentMetadataReadFailures_data()
    {
        QTest::addColumn<QByteArray>("metadata");
        QTest::addColumn<QByteArray>("indirectObject");
        QTest::newRow("description") << QByteArray("/Desc 7 0 R") << QByteArray("(description)");
        QTest::newRow("embedded-dictionary") << QByteArray("/Desc (description) /EF 7 0 R") << QByteArray("<< >>");
        QTest::newRow("stream-params") << QByteArray("/Desc (description) /EF << /F << /Params 7 0 R >> >>")
                                       << QByteArray("<< /Size 0 >>");
    }

    void attachmentMetadataReadFailures()
    {
        QFETCH(QByteArray, metadata);
        QFETCH(QByteArray, indirectObject);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("attachment-read-failure.pdf");
        const QByteArray name(1024, 'n');
        QVERIFY(writePdfObjects(path,
                                { "<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 5 0 R >> >>",
                                  "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
                                  "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> >>",
                                  "<< /Type /Filespec /F (" + name + ") " + metadata + " >>",
                                  "<< /Names [(attachment) 4 0 R] >>",
                                  "null",
                                  indirectObject }));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "attachment.pdf", &error), error.c_str());
        fz_context* context = document.context();
        pdf_document* pdf = pdf_specifics(context, document.document());
        // Cache the tree and filename while leaving the failing metadata object
        // unresolved. The old path allocated its C++ filename before this I/O.
        fz_try(context)
        {
            pdf_obj* tree = pdf_dict_getp(context, pdf_trailer(context, pdf), "Root/Names/EmbeddedFiles");
            pdf_obj* names = pdf_dict_get(context, tree, PDF_NAME(Names));
            pdf_obj* filespec = pdf_array_get(context, names, 1);
            (void)pdf_to_text_string(context, pdf_dict_get(context, filespec, PDF_NAME(F)));
        }
        fz_catch(context)
        {
            error = fz_caught_message(context);
        }
        QVERIFY2(error.empty(), error.c_str());
        const auto* exceptionTop = context->error.top;
        const auto seek = pdf->file->seek;
        for (int repeat = 0; repeat < 4; ++repeat) {
            error.clear();
            pdf->file->seek = failPdfSeek;
            const auto attachments = document.embeddedFiles(1024, 10, nullptr, &error);
            pdf->file->seek = seek;
            QVERIFY(attachments.empty());
            QVERIFY2(error.find("injected PDF read failure") != std::string::npos, error.c_str());
            QCOMPARE(context->error.top, exceptionTop);
        }
        error.clear();
        const auto attachments = document.embeddedFiles(1024, 10, nullptr, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(attachments.size(), size_t(1));
        QCOMPARE(attachments.front().name, name.toStdString());
    }

    void javaScriptActionReadFailures_data()
    {
        QTest::addColumn<QByteArray>("action");
        const QByteArray chain("<< /S /Named /N /NextPage /Next 8 0 R >>");
        QTest::newRow("primary") << QByteArray("/A ") + chain;
        QTest::newRow("mouse-down") << QByteArray("/AA << /D ") + chain + " >>";
        QTest::newRow("mouse-up") << QByteArray("/AA << /U ") + chain + " >>";
    }

    void javaScriptActionReadFailures()
    {
        QFETCH(QByteArray, action);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("action-read-failure.pdf");
        QVERIFY(writePdfObjects(
            path,
            { "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>",
              "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
              "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> /Annots [4 0 R] >>",
              "<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (Button) " + action
                  + " /Rect [50 50 150 90] /AP << /N 7 0 R >> >>",
              "<< /Fields [4 0 R] >>",
              "null",
              "<< /Type /XObject /Subtype /Form /BBox [0 0 100 40] /Resources << >> /Length 0 >>\nstream\nendstream",
              "<< /S /JavaScript /JS (void 0;) >>" }));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "actions.pdf", &error), error.c_str());
        // Load the widget with JS disabled, keeping its /Next target uncached.
        (void)document.pageGeometry(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        auto settings = document.settings();
        settings.formJavaScriptEnabled = true;
        document.setSettings(settings);
        fz_context* context = document.context();
        pdf_document* pdf = pdf_specifics(context, document.document());
        if (!pdf_js_supported(context, pdf))
            QSKIP("JavaScript is disabled in this MuPDF build");
        const auto* exceptionTop = context->error.top;
        const auto seek = pdf->file->seek;
        for (int repeat = 0; repeat < 4; ++repeat) {
            error.clear();
            pdf->file->seek = failPdfSeek;
            const auto details = document.pageDetails(0, &error, false);
            pdf->file->seek = seek;
            QVERIFY(details.formFields.empty());
            QVERIFY2(error.find("injected PDF read failure") != std::string::npos, error.c_str());
            QCOMPARE(context->error.top, exceptionTop);
        }
        error.clear();
        const auto details = document.pageDetails(0, &error, false);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.formFields.size(), size_t(1));
        QCOMPARE(details.formFields.front().pushButtonAction, ::Mu::Model::FormPushButtonAction::JavaScript);
    }

    void pageLinkLimitUnwindsResults_data()
    {
        QTest::addColumn<int>("count");
        const int limit = static_cast<int>(::Mu::Worker::Engine::Constant::MaxPageLinks);
        QTest::newRow("empty") << 0;
        QTest::newRow("at-limit") << limit;
        QTest::newRow("over-limit") << limit + 1;
    }

    void pageLinkLimitUnwindsResults()
    {
        QFETCH(int, count);
        const bool accepted = count <= static_cast<int>(::Mu::Worker::Engine::Constant::MaxPageLinks);
        const size_t expectedCount = accepted ? static_cast<size_t>(count) : 0;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("links.pdf");
        ::Mu::Worker::Engine::PdfDocument document;
        fz_context* context = document.context();
        createMultiPagePDF(context, path, 1);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "links.pdf", &error), error.c_str());
        (void)document.pageGeometry(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        pdf_page* volatile page = nullptr;
        fz_try(context)
        {
            page = pdf_load_page(context, pdf_specifics(context, document.document()), 0);
            // Inject a native list to test link extraction independently of the
            // annotation-count limit. Each long URI owns heap storage.
            for (int index = 0; index < count; ++index) {
                fz_link* link = pdf_new_link(context,
                                             page,
                                             { 0, 0, 1, 1 },
                                             "https://example.test/a-long-link-to-exercise-owned-uri-storage",
                                             nullptr);
                link->next = page->links;
                page->links = link;
            }
        }
        fz_always(context)
        {
            pdf_drop_page(context, page);
        }
        fz_catch(context)
        {
            error = fz_caught_message(context);
        }
        QVERIFY2(error.empty(), error.c_str());
        const auto* exceptionTop = context->error.top;
        const auto links = document.extractLinks(0, &error);
        QCOMPARE(links.size(), expectedCount);
        QCOMPARE(error, accepted ? std::string() : std::string("resource limit: page link limit exceeded"));
        QCOMPARE(context->error.top, exceptionTop);
        // Omitting error output must still discard the whole page on failure.
        const auto details = document.pageDetails(0, nullptr);
        QCOMPARE(details.links.size(), expectedCount);
        QCOMPARE(details.geometry.widthPoints, accepted ? 1.0 : 0.0);
        QCOMPARE(context->error.top, exceptionTop);
    }

    void embeddedFileLimits_data()
    {
        QTest::addColumn<int>("annotationCount");
        QTest::addColumn<int>("maxBytes");
        QTest::addColumn<int>("maxFiles");
        QTest::addColumn<bool>("nameTree");
        QTest::addColumn<int>("expectedFiles");
        QTest::newRow("byte-limit") << 2 << 7 << 2 << false << 0;
        QTest::newRow("file-limit") << 2 << 16 << 1 << false << 0;
        QTest::newRow("exact-budgets") << 2 << 16 << 2 << false << 2;
        QTest::newRow("annotation-limit")
            << static_cast<int>(::Mu::Worker::Engine::Constant::MaxPageAnnotations + 1) << 16 << 2 << false << 0;
        QTest::newRow("name-tree-partial") << 2 << 8 << 2 << true << 1;
    }

    void embeddedFileLimits()
    {
        QFETCH(int, annotationCount);
        QFETCH(int, maxBytes);
        QFETCH(int, maxFiles);
        QFETCH(bool, nameTree);
        QFETCH(int, expectedFiles);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("attachments.pdf");
        ::Mu::Worker::Engine::PdfDocument document;
        fz_context* context = document.context();
        QVERIFY(context);
        createMultiPagePDF(context, source, 1);
        QFile sourceFile(source);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY2(document.openFd(::dup(sourceFile.handle()), "attachments.pdf", &error), error.c_str());

        pdf_document* pdf = pdf_specifics(context, document.document());
        pdf_obj* volatile entries = nullptr;
        pdf_obj* volatile annotation = nullptr;
        pdf_obj* volatile filespec = nullptr;
        fz_buffer* volatile content = nullptr;
        fz_page* volatile page = nullptr;
        fz_try(context)
        {
            content = fz_new_buffer_from_copied_data(context, reinterpret_cast<const unsigned char*>("contents"), 8);
            filespec = pdf_add_embedded_file(context, pdf, "attachment.txt", "text/plain", content, 0, 0, 0);
            entries = pdf_new_array(context, pdf, annotationCount);
            for (int index = 0; index < annotationCount; ++index) {
                if (nameTree) {
                    pdf_array_push_drop(context, entries, pdf_new_text_string(context, "attachment.txt"));
                    pdf_array_push(context, entries, filespec);
                } else {
                    // Direct insertion keeps the annotation-limit fixture linear.
                    annotation = pdf_new_dict(context, pdf, 3);
                    pdf_dict_put(context,
                                 annotation,
                                 PDF_NAME(Subtype),
                                 annotationCount > 2 ? PDF_NAME(Text) : PDF_NAME(FileAttachment));
                    pdf_dict_put_rect(context, annotation, PDF_NAME(Rect), { 0, 0, 1, 1 });
                    pdf_dict_put(context, annotation, PDF_NAME(FS), filespec);
                    pdf_array_push_drop(context, entries, pdf_add_object(context, pdf, annotation));
                    pdf_drop_obj(context, annotation);
                    annotation = nullptr;
                }
            }
            if (nameTree)
                pdf_dict_putp(context,
                              pdf_dict_get(context, pdf_trailer(context, pdf), PDF_NAME(Root)),
                              "Names/EmbeddedFiles/Names",
                              entries);
            else
                pdf_dict_put(context, pdf_lookup_page_obj(context, pdf, 0), PDF_NAME(Annots), entries);
            page = fz_load_page(context, document.document(), 0);
        }
        fz_always(context)
        {
            pdf_drop_obj(context, annotation);
            pdf_drop_obj(context, entries);
            pdf_drop_obj(context, filespec);
            fz_drop_buffer(context, content);
        }
        fz_catch(context)
        {
            error = fz_caught_message(context);
        }
        QVERIFY2(page, error.c_str());
        const auto dropPage = [context](fz_page* ownedPage) {
            fz_drop_page(context, ownedPage);
        };
        const std::unique_ptr<fz_page, decltype(dropPage)> ownedPage(page, dropPage);
        const int pageRefs = page->refs;
        const auto* exceptionTop = context->error.top;
        // Exercise both forms of the optional limit output, repeatedly on one context.
        for (bool nullLimit : { false, true, false }) {
            bool limit = false;
            const auto files = document.embeddedFiles(static_cast<std::size_t>(maxBytes),
                                                      static_cast<std::size_t>(maxFiles),
                                                      nullLimit ? nullptr : &limit,
                                                      &error);
            QCOMPARE(context->error.top, exceptionTop);
            QCOMPARE(page->refs, pageRefs);
            QVERIFY2(error.empty(), error.c_str());
            QCOMPARE(files.size(), static_cast<std::size_t>(expectedFiles));
            if (!nullLimit)
                QCOMPARE(limit, expectedFiles < annotationCount);
        }
    }

    void formGraphLimits_data()
    {
        QTest::addColumn<QByteArray>("field");
        QTest::addColumn<QList<QByteArray>>("extraObjects");
        QTest::addColumn<bool>("accepted");
        QTest::addColumn<int>("selectedCount");
        const QByteArray named("/T (Field) ");
        QTest::newRow("single-value") << named + "/V (one)" << QList<QByteArray> { } << true << 1;
        QTest::newRow("flat-values") << named + "/V [(one) (two)]" << QList<QByteArray> { } << true << 2;
        QTest::newRow("missing-value") << named << QList<QByteArray> { } << true << 0;
        QTest::newRow("null-value") << named + "/V null" << QList<QByteArray> { } << true << 0;
        QTest::newRow("nested-array") << named + "/V [[(one)]]" << QList<QByteArray> { } << false << 0;
        QTest::newRow("self-cycle-value") << named + "/V 8 0 R" << QList<QByteArray> { "[8 0 R]" } << false << 0;
        QTest::newRow("two-node-cycle-value")
            << named + "/V 8 0 R" << QList<QByteArray> { "[9 0 R]", "[8 0 R]" } << false << 0;
        QTest::newRow("non-string-value") << named + "/V [42]" << QList<QByteArray> { } << false << 0;
        const int valueLimit = static_cast<int>(::Mu::Limit::MaxFormSelectedIndices);
        for (int count : { valueLimit, valueLimit + 1 }) {
            QByteArray values("/V [");
            for (int index = 0; index < count; ++index)
                values += "(one) ";
            values += "]";
            QTest::newRow(count == valueLimit ? "value-count-at-limit" : "value-count-over-limit")
                << named + values << QList<QByteArray> { } << (count == valueLimit) << count;
        }
        const int stringLimit = static_cast<int>(::Mu::Limit::MaxFormFieldStringBytes);
        QTest::newRow("value-string-over-limit")
            << named + "/V [(" + QByteArray(stringLimit + 1, 'x') + ")]" << QList<QByteArray> { } << false << 0;
        QTest::newRow("self-cycle-parent") << QByteArray("/Parent 4 0 R") << QList<QByteArray> { } << false << 0;
        QTest::newRow("two-node-cycle-parent")
            << QByteArray("/Parent 8 0 R") << QList<QByteArray> { "<< /Parent 4 0 R >>" } << false << 0;
        QTest::newRow("named-cycle-parent") << named + "/Parent 4 0 R" << QList<QByteArray> { } << false << 0;
        QTest::newRow("non-dictionary-parent") << named + "/Parent 8 0 R" << QList<QByteArray> { "42" } << false << 0;
        for (int depth : { 128, 129 }) {
            QList<QByteArray> parents;
            for (int index = 0; index < depth - 1; ++index)
                parents.push_back(index == depth - 2 ? QByteArray("<< /T (Owner) >>")
                                                     : "<< /Parent " + QByteArray::number(index + 9) + " 0 R >>");
            QTest::newRow(depth == 128 ? "parent-depth-at-limit" : "parent-depth-over-limit")
                << QByteArray("/Parent 8 0 R") << parents << (depth == 128) << 0;
        }
    }

    void formGraphLimits()
    {
        QFETCH(QByteArray, field);
        QFETCH(QList<QByteArray>, extraObjects);
        QFETCH(bool, accepted);
        QFETCH(int, selectedCount);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("form-graph.pdf");
        QList<QByteArray> objects {
            "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>",
            "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> /Annots [4 0 R] >>",
            "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /Opt [(one) (two)] " + field
                + " /Rect [50 50 150 90] /AP << /N 7 0 R >> >>",
            "<< /Fields [4 0 R] >>",
            "null",
            "<< /Type /XObject /Subtype /Form /BBox [0 0 100 40] /Resources << >> /Length 0 >>\nstream\nendstream"
        };
        objects.append(extraObjects);
        QVERIFY(writePdfObjects(path, objects));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "form-graph.pdf", &error), error.c_str());
        const auto* exceptionTop = document.context()->error.top;
        for (int repeat = 0; repeat < 2; ++repeat) {
            error.clear();
            const auto details = document.pageDetails(0, &error);
            QCOMPARE(document.context()->error.top, exceptionTop);
            QCOMPARE(error.empty(), accepted);
            QCOMPARE(details.formFields.size(), accepted ? 1u : 0u);
            if (accepted)
                QCOMPARE(details.formFields.front().currentChoices.size(), static_cast<std::size_t>(selectedCount));
        }
    }

    void embeddedTreeGraphLimits_data()
    {
        QTest::addColumn<QList<QByteArray>>("nodes");
        QTest::addColumn<bool>("limited");
        QTest::newRow("empty-leaf") << QList<QByteArray> { "<< /Names [] >>" } << false;
        QTest::newRow("self-cycle") << QList<QByteArray> { "<< /Kids [4 0 R] >>" } << true;
        QTest::newRow("branching-cycle") << QList<QByteArray> { "<< /Kids [4 0 R 4 0 R] >>" } << true;
        QTest::newRow("two-node-cycle") << QList<QByteArray> { "<< /Kids [5 0 R] >>", "<< /Kids [4 0 R] >>" } << true;
        QTest::newRow("shared-child") << QList<QByteArray> { "<< /Kids [5 0 R 5 0 R] >>", "<< /Names [] >>" } << true;
        const int depthLimit = ::Mu::Worker::Engine::Constant::MaxEmbeddedTreeDepth;
        for (int depth : { depthLimit, depthLimit + 1 }) {
            QList<QByteArray> nodes;
            for (int index = 0; index <= depth; ++index)
                nodes.push_back(index == depth ? QByteArray("<< /Names [] >>")
                                               : "<< /Kids [" + QByteArray::number(index + 5) + " 0 R] >>");
            QTest::newRow(depth == depthLimit ? "depth-at-limit" : "depth-over-limit") << nodes << (depth > depthLimit);
        }
        const int entryLimit = static_cast<int>(::Mu::Worker::Engine::Constant::MaxEmbeddedTreeEntries);
        // Invalid children and nameless files must still consume the work budget.
        for (bool names : { false, true }) {
            for (int count : { entryLimit - 1, entryLimit }) {
                QByteArray node(names ? "<< /Names [" : "<< /Kids [");
                for (int index = 0; index < count; ++index)
                    node += names ? "(ignored) null " : "null ";
                node += "] >>";
                const QByteArray row =
                    QByteArray(names ? "names-" : "kids-") + (count == entryLimit - 1 ? "at-limit" : "over-limit");
                QTest::newRow(row.constData()) << QList<QByteArray> { node } << (count == entryLimit);
            }
        }
    }

    void embeddedTreeGraphLimits()
    {
        QFETCH(QList<QByteArray>, nodes);
        QFETCH(bool, limited);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("attachment-graph.pdf");
        QList<QByteArray> objects { "<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 4 0 R >> >>",
                                    "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
                                    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> >>" };
        objects.append(nodes);
        QVERIFY(writePdfObjects(path, objects));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "attachment-graph.pdf", &error), error.c_str());
        const auto* exceptionTop = document.context()->error.top;
        for (bool nullLimit : { false, true, false }) {
            bool resourceLimit = false;
            const auto files = document.embeddedFiles(1024, 10, nullLimit ? nullptr : &resourceLimit, &error);
            QVERIFY2(error.empty(), error.c_str());
            QVERIFY(files.empty());
            if (!nullLimit)
                QCOMPARE(resourceLimit, limited);
            QCOMPARE(document.context()->error.top, exceptionTop);
        }
    }

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

    void printAppearanceVisibility_data()
    {
        QTest::addColumn<QByteArray>("subtype");
        QTest::addColumn<int>("flags");
        QTest::addColumn<int>("rotation");
        QTest::addColumn<int>("userUnit");
        QTest::addColumn<bool>("layerPrints");
        QTest::addColumn<bool>("drawn");
        for (const QByteArray& subtype : { QByteArray("Square"), QByteArray("Widget") }) {
            const auto row = [&](const char* name,
                                 int flags,
                                 bool drawn,
                                 int rotation = 0,
                                 int userUnit = 1,
                                 bool layerPrints = true) {
                const QByteArray label = subtype + "-" + name;
                QTest::newRow(label.constData()) << subtype << flags << rotation << userUnit << layerPrints << drawn;
            };
            row("printable", PDF_ANNOT_IS_PRINT, true);
            row("screen-only", 0, false);
            row("hidden", PDF_ANNOT_IS_PRINT | PDF_ANNOT_IS_HIDDEN, false);
            row("invisible", PDF_ANNOT_IS_PRINT | PDF_ANNOT_IS_INVISIBLE, false);
            row("print-only", PDF_ANNOT_IS_PRINT | PDF_ANNOT_IS_NO_VIEW, true);
            row("rotated", PDF_ANNOT_IS_PRINT, true, 90);
            row("no-rotate", PDF_ANNOT_IS_PRINT | PDF_ANNOT_IS_NO_ROTATE, true, 90);
            row("no-zoom", PDF_ANNOT_IS_PRINT | PDF_ANNOT_IS_NO_ZOOM, true, 270, 2);
            row("no-rotate-no-zoom", PDF_ANNOT_IS_PRINT | PDF_ANNOT_IS_NO_ROTATE | PDF_ANNOT_IS_NO_ZOOM, true, 270, 2);
            row("layer-print-off", PDF_ANNOT_IS_PRINT, false, 0, 1, false);
        }
        QTest::newRow("popup") << QByteArray("Popup") << int(PDF_ANNOT_IS_PRINT) << 0 << 1 << true << false;
        QTest::newRow("attachment") << QByteArray("FileAttachment") << int(PDF_ANNOT_IS_PRINT) << 0 << 1 << true
                                    << false;
    }

    void printAppearanceVisibility()
    {
        QFETCH(QByteArray, subtype);
        QFETCH(int, flags);
        QFETCH(int, rotation);
        QFETCH(int, userUnit);
        QFETCH(bool, layerPrints);
        QFETCH(bool, drawn);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("source.pdf");
        const QString target = directory.filePath("print.pdf");
        const QByteArray appearance("0.8 0.1 0.2 rg 0 0 60 20 re f\n");
        const QByteArray content("0.2 0.3 0.4 rg 30 20 10 10 re f\n2 0 0 2 0 0 cm\n0 0 1 1 re W n\n");
        QList<QByteArray> objects {
            "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R /OCProperties << /OCGs [8 0 R] "
            "/D << /ON [8 0 R] /AS [<< /Event /Print /Category [/Print] /OCGs [8 0 R] >>] >> >> >>",
            "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 160 120] /CropBox [20 10 140 110] /Rotate "
                + QByteArray::number(rotation) + " /UserUnit " + QByteArray::number(userUnit)
                + " /Resources << >> /Contents 6 0 R /Annots [4 0 R] >>",
            "<< /Type /Annot /Subtype /" + subtype + " /FT /Tx /T (Field) /V (Old) /F " + QByteArray::number(flags)
                + " /Rect [50 50 110 70] /OC 8 0 R /AP << /N 7 0 R >> >>",
            "<< /Fields [4 0 R] >>",
            "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream",
            "<< /Type /XObject /Subtype /Form /BBox [0 0 60 20] /Resources << >> /Length "
                + QByteArray::number(appearance.size()) + " >>\nstream\n" + appearance + "endstream",
            QByteArray("<< /Type /OCG /Name (Layer) /Usage << /Print << /PrintState /") + (layerPrints ? "ON" : "OFF")
                + " >> >> >>"
        };
        QVERIFY(writePdfObjects(source, objects));
        QFile file(source);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "source.pdf", &error), error.c_str());
        const auto expected = renderPdfUsage(document, 0, "Print", &error);
        const auto viewBefore = renderPdfUsage(document, 0, "View", &error);
        QVERIFY2(!expected.empty() && error.empty(), error.c_str());
        objects[2].replace("/Annots [4 0 R]", "/Annots []");
        const QString baseline = directory.filePath("baseline.pdf");
        QVERIFY(writePdfObjects(baseline, objects));
        QFile baselineFile(baseline);
        QVERIFY(baselineFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument blank;
        QVERIFY2(blank.openFd(::dup(baselineFile.handle()), "baseline.pdf", &error), error.c_str());
        QCOMPARE(expected != renderPdfUsage(blank, 0, "Print", &error), drawn);

        const int fd = ::open(QFile::encodeName(target).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fd >= 0);
        QVERIFY2(document.savePdfFd(fd, { }, &error), error.c_str());
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        QCOMPARE(renderPdfUsage(document, 0, "View", &error), viewBefore);
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument printed;
        QVERIFY2(printed.openFd(::dup(output.handle()), "print.pdf", &error), error.c_str());
        const auto geometry = document.pageGeometry(0);
        const auto printedGeometry = printed.pageGeometry(0);
        QCOMPARE(printedGeometry.widthPoints, geometry.widthPoints);
        QCOMPARE(printedGeometry.heightPoints, geometry.heightPoints);
        QVERIFY(pixelsMatch(renderPdfUsage(printed, 0, "View", &error), expected));
        QVERIFY(printed.pageDetails(0, &error).formFields.empty());
        QVERIFY(printed.extractAnnotations(0, &error).empty());
    }

    void printLiveFormValues()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("form.pdf");
        const QString target = directory.filePath("print.pdf");
        ::Mu::Worker::Engine::PdfDocument document;
        createEditableTextFieldPDF(document.context(), source);
        QFile file(source);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll();
        file.seek(0);
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "form.pdf", &error), error.c_str());
        const auto field = document.pageDetails(0, &error).formFields.front();
        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY2(
            document.updateFormField(
                0, field.pdfObjectNumber, ::Mu::Model::FormTextValue { "Unsaved print value" }, &mutations, &error),
            error.c_str());
        const auto expected = renderPdfUsage(document, 0, "Print", &error);
        const int fd = ::open(QFile::encodeName(target).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fd >= 0);
        QVERIFY2(document.savePdfFd(fd, { 0 }, &error), error.c_str());
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        QCOMPARE(document.pageDetails(0, &error).formFields.front().text, std::string("Unsaved print value"));
        QCOMPARE(renderPdfUsage(document, 0, "Print", &error), expected);
        QVERIFY(pdf_has_unsaved_changes(document.context(), pdf_specifics(document.context(), document.document())));
        file.seek(0);
        QCOMPARE(file.readAll(), original);
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument printed;
        QVERIFY2(printed.openFd(::dup(output.handle()), "print.pdf", &error), error.c_str());
        QVERIFY(pixelsMatch(renderPdfUsage(printed, 0, "View", &error), expected));
        QVERIFY(!printed.textBoxes(0, 72, 72, 1000, true, &error).empty());
        const int fullFd = ::open("/dev/full", O_WRONLY);
        QVERIFY(fullFd >= 0);
        QVERIFY(!document.savePdfFd(fullFd, { }, &error));
        QVERIFY(!error.empty());
        QVERIFY(::fcntl(fullFd, F_GETFD) == -1 && errno == EBADF);
        error.clear();
        QVERIFY2(document.updateFormField(
                     0, field.pdfObjectNumber, ::Mu::Model::FormTextValue { "Still editable" }, &mutations, &error),
                 error.c_str());
    }

    void printPageSelection_data()
    {
        QTest::addColumn<std::vector<int>>("pages");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("all") << std::vector<int> { } << true;
        QTest::newRow("subset") << std::vector<int> { 1 } << true;
        QTest::newRow("reordered") << std::vector<int> { 1, 0 } << true;
        QTest::newRow("negative") << std::vector<int> { -1 } << false;
        QTest::newRow("out-of-range") << std::vector<int> { 2 } << false;
        QTest::newRow("too-many") << std::vector<int> { 0, 1, 0 } << false;
    }

    void printPageSelection()
    {
        QFETCH(std::vector<int>, pages);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("source.pdf");
        const QString target = directory.filePath("print.pdf");
        const QByteArray pattern("1 0 0 rg 0 0 4 4 re f\n");
        const QByteArray content("/Pattern cs /Tiles scn 0 0 80 60 re f\n");
        const QByteArray second("0 0 1 rg 0 0 40 30 re f\n");
        QVERIFY(writePdfObjects(
            source,
            { "<< /Type /Catalog /Pages 2 0 R /Names << /Private (unselected document metadata) >> >>",
              "<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
              "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 80 60] /Resources << /Pattern << /Tiles 7 0 R >> >> "
              "/Contents 5 0 R >>",
              "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 60 80] /Resources << >> /Contents 6 0 R >>",
              "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream",
              "<< /Length " + QByteArray::number(second.size()) + " >>\nstream\n" + second + "endstream",
              "<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 8 8] /XStep 8 /YStep 8 "
              "/Resources << >> /Length "
                  + QByteArray::number(pattern.size()) + " >>\nstream\n" + pattern + "endstream" }));
        QFile file(source);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(document.openFd(::dup(file.handle()), "source.pdf", &error), error.c_str());
        const int fd = ::open(QFile::encodeName(target).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fd >= 0);
        QCOMPARE(document.savePdfFd(fd, pages, &error), accepted);
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        if (!accepted) {
            QVERIFY(!error.empty());
            return;
        }
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument printed;
        QVERIFY2(printed.openFd(::dup(output.handle()), "print.pdf", &error), error.c_str());
        if (pages.empty())
            pages = { 0, 1 };
        QCOMPARE(printed.pageCount(), static_cast<int>(pages.size()));
        QVERIFY(!pdf_dict_getp(printed.context(),
                               pdf_trailer(printed.context(), pdf_specifics(printed.context(), printed.document())),
                               "Root/Names"));
        for (std::size_t index = 0; index < pages.size(); ++index)
            QVERIFY(pixelsMatch(renderPdfUsage(printed, static_cast<int>(index), "View", &error),
                                renderPdfUsage(document, pages[index], "Print", &error)));
    }

    void flattenPreservesLiveEdits_data()
    {
        QTest::addColumn<bool>("form");
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("highlight") << false << false;
        QTest::newRow("filled-form") << true << false;
        QTest::newRow("encrypted-highlight") << false << true;
    }

    void flattenPreservesLiveEdits()
    {
        QFETCH(bool, form);
        QFETCH(bool, encrypted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath("source.pdf");
        const QString target = directory.filePath("flattened.pdf");
        ::Mu::Worker::Engine::PdfDocument document;
        fz_context* context = document.context();
        if (form)
            createEditableTextFieldPDF(context, source);
        else if (encrypted) {
            const QString plain = directory.filePath("plain.pdf");
            createTextPDF(context, plain);
            pdf_document* pdf = pdf_open_document(context, QFile::encodeName(plain).constData());
            pdf_write_options options = pdf_default_write_options;
            options.do_encrypt = PDF_ENCRYPT_AES_128;
            std::strcpy(options.upwd_utf8, "secret");
            std::strcpy(options.opwd_utf8, "secret");
            pdf_save_document(context, pdf, QFile::encodeName(source).constData(), &options);
            pdf_drop_document(context, pdf);
        } else
            createTextPDF(context, source);
        QFile input(source);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QByteArray original = input.readAll();
        input.seek(0);
        std::string error;
        QVERIFY2(document.openFd(::dup(input.handle()), "source.pdf", &error), error.c_str());
        if (encrypted)
            QVERIFY2(document.unlock("secret", &error), error.c_str());
        const auto before = renderPdfPage(document, 0, 612, 792, &error);
        if (form) {
            const auto fields = document.pageDetails(0, &error).formFields;
            QCOMPARE(fields.size(), size_t(1));
            std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
            QVERIFY2(document.updateFormField(0,
                                              fields.front().pdfObjectNumber,
                                              ::Mu::Model::FormTextValue { "Flatten test" },
                                              &mutations,
                                              &error),
                     error.c_str());
        } else {
            ::Mu::Model::Annotation annotation;
            annotation.subtype = ::Mu::Model::AnnotationType::Highlight;
            annotation.uuid = "flatten-highlight";
            annotation.x0 = .1;
            annotation.y0 = .3;
            annotation.x1 = .4;
            annotation.y1 = .35;
            annotation.color = 0xffffff00U;
            annotation.extras.quads.push_back({ { .1, .3 }, { .4, .3 }, { .4, .35 }, { .1, .35 } });
            std::int32_t object = -1;
            QVERIFY2(document.addAnnotation(0, annotation, &object, &error), error.c_str());
        }
        const auto edited = renderPdfPage(document, 0, 612, 792, &error);
        QVERIFY(!edited.empty());
        QVERIFY(before != edited);
        pdf_document* live = pdf_specifics(context, document.document());
        QVERIFY(pdf_has_unsaved_changes(context, live));
        const auto liveDetails = document.pageDetails(0, &error);
        const int fd = ::open(QFile::encodeName(target).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fd >= 0);
        QVERIFY2(document.flattenPdfFd(fd, { }, &error), error.c_str());
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        QVERIFY(pdf_has_unsaved_changes(context, live));
        QCOMPARE(renderPdfPage(document, 0, 612, 792, &error), edited);
        QCOMPARE(document.pageDetails(0, &error).formFields.size(), liveDetails.formFields.size());
        QCOMPARE(document.extractAnnotations(0, &error).size(), liveDetails.annotations.size());
        input.seek(0);
        QCOMPARE(input.readAll(), original);

        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reloaded;
        QVERIFY2(reloaded.openFd(::dup(output.handle()), "flattened.pdf", &error), error.c_str());
        QVERIFY(!reloaded.isLocked());
        QVERIFY(reloaded.pageDetails(0, &error).formFields.empty());
        QVERIFY(reloaded.extractAnnotations(0, &error).empty());
        const auto flattened = renderPdfPage(reloaded, 0, 612, 792, &error);
        // Moving an appearance into page content can change raster rounding
        // by up to two channel levels; missing or displaced content must fail.
        QVERIFY(flattened.size() == edited.size());
        QVERIFY(std::equal(flattened.begin(), flattened.end(), edited.begin(), [](auto actual, auto expected) {
            return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 2;
        }));
        QVERIFY(!reloaded.textBoxes(0, 72, 72, 1000, true, &error).empty());

        // A failed write must also leave the live annotations and fields intact.
        const int fullFd = ::open("/dev/full", O_WRONLY);
        QVERIFY(fullFd >= 0);
        QVERIFY(!document.flattenPdfFd(fullFd, { }, &error));
        QVERIFY(::fcntl(fullFd, F_GETFD) == -1 && errno == EBADF);
        error.clear();
        QCOMPARE(renderPdfPage(document, 0, 612, 792, &error), edited);
        QVERIFY(pdf_has_unsaved_changes(context, live));
        if (form) {
            const auto field = document.pageDetails(0, &error).formFields.front();
            std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
            QVERIFY(document.updateFormField(
                0, field.pdfObjectNumber, ::Mu::Model::FormTextValue { "Still editable" }, &mutations, &error));
        } else {
            const auto annotations = document.extractAnnotations(0, &error);
            QVERIFY(!annotations.empty());
            QVERIFY(document.removeAnnotation(0, annotations.front().pdfObjectNumber, &error));
        }
    }

    void flattenUnavailableDocument_data()
    {
        QTest::addColumn<QString>("state");
        QTest::newRow("closed") << QStringLiteral("closed");
        QTest::newRow("locked") << QStringLiteral("locked");
        QTest::newRow("xfa") << QStringLiteral("xfa");
    }

    void flattenUnavailableDocument()
    {
        QFETCH(QString, state);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ::Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY(!document.flattenPdfFd(-1, { }, &error));
        if (state != QLatin1String("closed")) {
            const QString source = directory.filePath("source.pdf");
            if (state == QLatin1String("locked"))
                createEncryptedPDF(document.context(), source, QStringLiteral("secret"));
            else
                createTextPDF(document.context(), source);
            QFile input(source);
            QVERIFY(input.open(QIODevice::ReadOnly));
            QVERIFY(document.openFd(::dup(input.handle()), "source.pdf", &error));
            if (state == QLatin1String("xfa")) {
                fz_context* context = document.context();
                pdf_document* pdf = pdf_specifics(context, document.document());
                pdf_obj* root = pdf_dict_get(context, pdf_trailer(context, pdf), PDF_NAME(Root));
                pdf_obj* form = pdf_dict_put_dict(context, root, PDF_NAME(AcroForm), 1);
                pdf_dict_put_text_string(context, form, PDF_NAME(XFA), "unsupported XFA data");
            }
        }
        const int fd =
            ::open(QFile::encodeName(directory.filePath("export.pdf")).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fd >= 0);
        error.clear();
        QVERIFY(!document.flattenPdfFd(fd, { }, &error));
        QVERIFY(!error.empty());
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        if (state == QLatin1String("xfa"))
            QCOMPARE(document.metadata({ "hasXfaForm" }).values.at("hasXfaForm"), std::string("true"));
    }

    void flattenPreservesLayers()
    {
        ::Mu::Worker::Engine::PdfDocument document;
        QFile input(QStringLiteral(TEST_SIGNATURE_PDF_DIR "/layers.pdf"));
        QVERIFY(input.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY(document.openFd(::dup(input.handle()), "layers.pdf", &error));
        const auto layers = document.layers(&error);
        QVERIFY(!layers.empty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString target = directory.filePath("layers.pdf");
        const int fd = ::open(QFile::encodeName(target).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fd >= 0);
        QVERIFY2(document.flattenPdfFd(fd, { }, &error), error.c_str());
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reloaded;
        QVERIFY(reloaded.openFd(::dup(output.handle()), "layers.pdf", &error));
        const auto exportedLayers = reloaded.layers(&error);
        QCOMPARE(exportedLayers.size(), layers.size());
        for (size_t index = 0; index < layers.size(); ++index) {
            QCOMPARE(exportedLayers[index].name, layers[index].name);
            QCOMPARE(exportedLayers[index].selected, layers[index].selected);
        }
        QCOMPARE(renderPdfPage(reloaded, 0, 612, 792, &error), renderPdfPage(document, 0, 612, 792, &error));
    }

    void flattenPageSelection_data()
    {
        QTest::addColumn<QList<int>>("selection");
        QTest::addColumn<bool>("success");
        QTest::newRow("all") << QList<int> { } << true;
        QTest::newRow("one") << QList<int> { 1 } << true;
        QTest::newRow("reordered") << QList<int> { 1, 0 } << true;
        QTest::newRow("negative") << QList<int> { -1 } << false;
        QTest::newRow("out-of-range") << QList<int> { 2 } << false;
        QTest::newRow("too-many") << QList<int> { 0, 1, 0 } << false;
    }

    void flattenPageSelection()
    {
        QFETCH(QList<int>, selection);
        QFETCH(bool, success);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ::Mu::Worker::Engine::PdfDocument document;
        const QString source = directory.filePath("pages.pdf");
        createMultiPagePDF(document.context(), source, 2);
        QFile input(source);
        QVERIFY(input.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY(document.openFd(::dup(input.handle()), "pages.pdf", &error));
        fz_set_metadata(document.context(), document.document(), "info:Title", "Flattened title");
        fz_outline_iterator* iterator = fz_new_outline_iterator(document.context(), document.document());
        char title[] = "Second page";
        char uri[] = "#page=2";
        fz_outline_item item { };
        item.title = title;
        item.uri = uri;
        fz_outline_iterator_insert(document.context(), iterator, &item);
        fz_drop_outline_iterator(document.context(), iterator);
        pdf_page* page = pdf_load_page(document.context(), pdf_specifics(document.context(), document.document()), 1);
        fz_link* link = fz_create_link(
            document.context(), reinterpret_cast<fz_page*>(page), { 10, 10, 50, 50 }, "https://example.com/");
        fz_drop_link(document.context(), link);
        pdf_drop_page(document.context(), page);
        const QString target = directory.filePath("export.pdf");
        const int fd = ::open(QFile::encodeName(target).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        QVERIFY(fd >= 0);
        const std::vector<int> pages(selection.begin(), selection.end());
        QCOMPARE(document.flattenPdfFd(fd, pages, &error), success);
        QVERIFY(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        QCOMPARE(document.pageCount(), 2);
        QCOMPARE(document.outline().size(), size_t(1));
        if (!success)
            return;
        QFile output(target);
        QVERIFY(output.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reloaded;
        QVERIFY2(reloaded.openFd(::dup(output.handle()), "export.pdf", &error), error.c_str());
        QCOMPARE(reloaded.pageCount(), selection.isEmpty() ? 2 : static_cast<int>(selection.size()));
        QCOMPARE(reloaded.metadata({ "title" }).values.at("title"), std::string("Flattened title"));
        QCOMPARE(reloaded.outline().size(), size_t(1));
        const int linkPage = selection.isEmpty() ? 1 : static_cast<int>(selection.indexOf(1));
        QCOMPARE(reloaded.extractLinks(linkPage).size(), size_t(1));
        for (int index = 0; index < reloaded.pageCount(); ++index) {
            const int originalPage = selection.isEmpty() ? index : selection.at(index);
            QCOMPARE(renderPdfPage(reloaded, index, 612, 792, &error),
                     renderPdfPage(document, originalPage, 612, 792, &error));
        }
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

        ::Mu::Model::Annotation unsupported;
        unsupported.subtype = ::Mu::Model::AnnotationType::Unknown;
        std::int32_t unsupportedObject = -1;
        QVERIFY(!document.addAnnotation(0, unsupported, &unsupportedObject, &error));

        const auto before = renderPdfPage(document, 0, 612, 792, &error);
        QVERIFY2(!before.empty(), error.c_str());
        ::Mu::Model::Annotation annotation;
        annotation.subtype = ::Mu::Model::AnnotationType::Highlight;
        annotation.uuid = "highlight-round-trip";
        annotation.x0 = .1;
        annotation.y0 = .3;
        annotation.x1 = .4;
        annotation.y1 = .35;
        annotation.color = 0xffffff00U;
        annotation.flags = ::Mu::Model::annotationFlagValue(::Mu::Model::AnnotationFlag::Print);
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
            annotation.subtype = ::Mu::Model::AnnotationType::Highlight;
            annotation.uuid = "stress-" + std::to_string(i);
            annotation.x0 = x0;
            annotation.y0 = y0;
            annotation.x1 = x0 + .07;
            annotation.y1 = y0 + .03;
            annotation.color = i % 2 ? 0xffffff00U : 0xff00ff00U;
            annotation.flags = ::Mu::Model::annotationFlagValue(::Mu::Model::AnnotationFlag::Print);
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
        annotation.subtype = ::Mu::Model::AnnotationType::Highlight;
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

    void lineAnnotationRequiresTwoPoints()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("lines.pdf"));
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, path);
        fz_drop_context(context);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        ::Mu::Worker::Runtime::CommandService service({ });
        const auto opened = service.openFdResponse(1, ::dup(file.handle()), "lines.pdf");
        QVERIFY2(!opened.error, opened.error ? opened.error->message.c_str() : "");

        const auto lineCount = [&]() -> std::ptrdiff_t {
            const auto annotations = service.document()->extractAnnotations(0, nullptr);
            return std::count_if(annotations.begin(), annotations.end(), [](const ::Mu::Model::Annotation& value) {
                return value.subtype == ::Mu::Model::AnnotationType::Line;
            });
        };

        ::Mu::Model::Annotation line;
        line.subtype = ::Mu::Model::AnnotationType::Line;
        line.x0 = .1;
        line.y0 = .1;
        line.x1 = .5;
        line.y1 = .5;

        // Zero and one points both lack geometry and must not create anything.
        const auto emptyAdd = service.dispatch({ 2, ::Mu::Model::AnnotationAddRequest { 0, line } });
        QVERIFY(emptyAdd.error);
        line.extras.points.push_back({ .1, .1 });
        const auto singleAdd = service.dispatch({ 3, ::Mu::Model::AnnotationAddRequest { 0, line } });
        QVERIFY(singleAdd.error);
        QCOMPARE(lineCount(), std::ptrdiff_t(0));

        // A two-point line is accepted, but degrading it to one point fails and
        // leaves the original two-point geometry intact.
        line.extras.points.push_back({ .5, .5 });
        const auto added = service.dispatch({ 4, ::Mu::Model::AnnotationAddRequest { 0, line } });
        QVERIFY2(!added.error, added.error ? added.error->message.c_str() : "");
        const auto handle = std::get<::Mu::Model::AnnotationResponse>(added.payload).handle;
        QVERIFY(!handle.value.empty());
        QCOMPARE(lineCount(), std::ptrdiff_t(1));

        auto degenerate = line;
        degenerate.extras.points.pop_back();
        const auto rejectedModify =
            service.dispatch({ 5, ::Mu::Model::AnnotationModifyRequest { { 0, handle, degenerate, true } } });
        QVERIFY(rejectedModify.error);

        std::string error;
        const auto annotations = service.document()->extractAnnotations(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(annotations.size(), size_t(1));
        QCOMPARE(annotations.front().extras.points.size(), size_t(2));
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
              .existingFieldObjectNumber = fields.front().objectNumber,
              .appearance = { } },
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

    void deferredQueueBounds()
    {
        // Frame-count bound + recovery
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

        // FIFO order on an independent service
        ::Mu::Worker::Runtime::CommandService fifo({ });
        const std::vector<std::byte> first(3, std::byte { 0x01 });
        const std::vector<std::byte> second(5, std::byte { 0x02 });
        QVERIFY(fifo.deferIncoming(first));
        QVERIFY(fifo.deferIncoming(second));

        const auto firstTaken = fifo.takeDeferredIncoming();
        QVERIFY(firstTaken.has_value());
        QCOMPARE(*firstTaken, first);
        const auto secondTaken = fifo.takeDeferredIncoming();
        QVERIFY(secondTaken.has_value());
        QCOMPARE(*secondTaken, second);
        QVERIFY(!fifo.takeDeferredIncoming().has_value());

        // Byte bound
        std::vector<std::byte> oversized(::Mu::Worker::Runtime::MaxDeferredBytes + 1, std::byte { 0x03 });
        QVERIFY(!fifo.deferIncoming(std::move(oversized)));
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

    void extractFormFieldsStringLimits_data()
    {
        QTest::addColumn<QByteArray>("key");
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<bool>("accepted");
        const int nameLimit = static_cast<int>(::Mu::Limit::MaxFormNameBytes);
        const int textLimit = static_cast<int>(::Mu::Limit::MaxFormFieldStringBytes);
        QTest::newRow("name-at-limit") << QByteArray("T") << QByteArray(nameLimit, 'X') << true;
        QTest::newRow("name-over-limit") << QByteArray("T") << QByteArray(nameLimit + 1, 'X') << false;
        QTest::newRow("label-over-name-limit") << QByteArray("TU") << QByteArray(nameLimit + 1, 'X') << true;
        QTest::newRow("label-at-limit") << QByteArray("TU") << QByteArray(textLimit, 'X') << true;
        QTest::newRow("label-over-limit") << QByteArray("TU") << QByteArray(textLimit + 1, 'X') << false;
        QTest::newRow("label-utf8-byte-limit")
            << QByteArray("TU") << (QByteArray(textLimit - 1, 'X') + QByteArray("\xc3\xa9")) << false;
    }

    void extractFormFieldsStringLimits()
    {
        QFETCH(QByteArray, key);
        QFETCH(QByteArray, value);
        QFETCH(bool, accepted);
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

            pdf_dict_put_text_string(ctx, widgetObj, PDF_NAME(T), "field");
            pdf_dict_puts_drop(ctx, widgetObj, key.constData(), pdf_new_text_string(ctx, value.constData()));

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
        std::string warnings;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY(doc.openFd(::dup(file.handle()), "oversized_form.pdf", &error));
        fz_set_warning_callback(
            doc.context(),
            [](void* user, const char* message) { *static_cast<std::string*>(user) += message; },
            &warnings);
        const auto details = doc.pageDetails(0, &error);
        QVERIFY2(warnings.find("UNHANDLED EXCEPTION") == std::string::npos, warnings.c_str());
        if (accepted) {
            QVERIFY2(error.empty(), error.c_str());
            QCOMPARE(details.formFields.size(), 1u);
            const auto& field = details.formFields.front();
            QCOMPARE(key == "TU" ? field.uiName : field.partialName, value.toStdString());
        } else {
            QVERIFY(QString::fromStdString(error).contains(QStringLiteral("resource limit")));
        }
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

    void formMutationsCollectOnlyEditableWidgets_data()
    {
        QTest::addColumn<bool>("shared");
        QTest::addColumn<bool>("extras");
        QTest::newRow("single-widget") << false << false;
        QTest::newRow("shared-widgets") << true << false;
        QTest::newRow("shared-with-unrelated-data") << true << true;
    }

    void formMutationsCollectOnlyEditableWidgets()
    {
        QFETCH(bool, shared);
        QFETCH(bool, extras);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Three pages: an edited widget, an optional sibling of the same
        // logical field, and a page without fields. Extras exercise extraction
        // parity with annotations, links, signatures, and a push button present.
        const QList<QByteArray> objects {
            "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [8 0 R " + QByteArray(extras ? "11 0 R 12 0 R" : "")
                + "] /DR << /Font << /Helv 14 0 R >> >> >> >>",
            "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources <<>> /Contents 13 0 R /Annots [6 0 R "
                + QByteArray(extras ? "9 0 R 10 0 R 11 0 R 12 0 R" : "") + "] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources <<>> /Contents 13 0 R /Annots ["
                + QByteArray(shared ? "7 0 R" : "") + "] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources <<>> /Contents 13 0 R >>",
            "<< /Type /Annot /Subtype /Widget /Parent 8 0 R /P 3 0 R /Rect [10 10 90 30] >>",
            "<< /Type /Annot /Subtype /Widget /Parent 8 0 R /P 4 0 R /Rect [10 10 90 30] >>",
            "<< /FT /Tx /T (Shared) /V (initial) /DA (/Helv 10 Tf 0 g) /Kids [6 0 R "
                + QByteArray(shared ? "7 0 R" : "") + "] >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 40 30 60] /Contents (note) >>",
            "<< /Type /Annot /Subtype /Link /Rect [40 40 60 60] /A << /S /URI /URI (https://example.com) >> >>",
            "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature) /P 3 0 R /Rect [10 65 40 85] >>",
            "<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (Button) /P 3 0 R /Rect [50 65 90 85] >>",
            "<< /Length 0 >>\nstream\n\nendstream",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
        };
        QByteArray data("%PDF-1.7\n");
        QByteArray xref("0000000000 65535 f \n");
        for (qsizetype i = 0; i < objects.size(); ++i) {
            xref += QByteArray::number(data.size()).rightJustified(10, '0') + " 00000 n \n";
            data += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const auto xrefOffset = data.size();
        data += "xref\n0 15\n" + xref + "trailer\n<< /Size 15 /Root 1 0 R >>\nstartxref\n"
            + QByteArray::number(xrefOffset) + "\n%%EOF\n";
        QFile file(dir.filePath(QStringLiteral("mutations.pdf")));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QCOMPARE(file.write(data), data.size());
        QVERIFY(file.flush());

        std::string error;
        ::Mu::Worker::Engine::PdfDocument document;
        QVERIFY2(document.openFd(::dup(file.handle()), "mutations.pdf", &error), error.c_str());
        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY2(document.updateFormField(0, 6, ::Mu::Model::FormTextValue { "updated" }, &mutations, &error),
                 error.c_str());
        QCOMPARE(mutations.size(), shared ? size_t(2) : size_t(1));
        for (std::size_t i = 0; i < mutations.size(); ++i) {
            QCOMPARE(mutations[i].page, static_cast<int>(i));
            QCOMPARE(mutations[i].objectNumber, 6 + static_cast<int>(i));
            const auto* value = std::get_if<::Mu::Model::FormTextValue>(&mutations[i].actualValue);
            QVERIFY(value);
            QCOMPARE(value->text, std::string("updated"));
            const auto details = document.pageDetails(static_cast<int>(i), &error);
            QVERIFY2(error.empty(), error.c_str());
            QCOMPARE(details.formFields.front().text, value->text);
            if (extras && i == 0) {
                QVERIFY(!details.annotations.empty());
                QCOMPARE(details.links.size(), size_t(1));
                QCOMPARE(details.signatures.size(), size_t(1));
                QCOMPARE(details.formFields.size(), size_t(2));
                QCOMPARE(details.formFields.back().type, ::Mu::Model::FormFieldType::PushButton);
            }
        }
        QVERIFY(document.pageDetails(2, &error).formFields.empty());
        QVERIFY2(error.empty(), error.c_str());
    }

    void formJavaScriptCalculatesDependentField()
    {
        QFile file(QStringLiteral(FORM_JS_PDF_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(file.handle()), "form_javascript_calculation.pdf", &error), error.c_str());

        pdf_document* pdfDoc = pdf_specifics(doc.context(), doc.document());
        QVERIFY(pdfDoc);
        if (!pdf_js_supported(doc.context(), pdfDoc))
            QSKIP("JavaScript is disabled in this MuPDF build");

        auto details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto quantity = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Quantity";
        });
        QVERIFY(quantity != details.formFields.end());

        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY2(
            doc.updateFormField(0, quantity->pdfObjectNumber, ::Mu::Model::FormTextValue { "4" }, &mutations, &error),
            error.c_str());

        details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        auto total = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Total";
        });
        QVERIFY(total != details.formFields.end());
        QCOMPARE(total->text, std::string("71.82"));
        const auto totalMutation = std::find_if(mutations.begin(), mutations.end(), [&](const auto& mutation) {
            return mutation.objectNumber == total->pdfObjectNumber;
        });
        QVERIFY(totalMutation != mutations.end());
        const auto* totalValue = std::get_if<::Mu::Model::FormTextValue>(&totalMutation->actualValue);
        QVERIFY(totalValue);
        QCOMPARE(totalValue->text, total->text);

        const auto discount = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "DiscountPercent";
        });
        QVERIFY(discount != details.formFields.end());
        QVERIFY2(
            doc.updateFormField(0, discount->pdfObjectNumber, ::Mu::Model::FormTextValue { "101" }, &mutations, &error),
            error.c_str());

        details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        total = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Total";
        });
        QVERIFY(total != details.formFields.end());
        QCOMPARE(total->text, std::string("Invalid input"));
    }

    void formJavaScriptPushButtonRunsClickAction()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(QStringLiteral(FORM_JS_BUTTON_PDF_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(file.handle()), "form_javascript_button.pdf", &error), error.c_str());

        pdf_document* pdfDoc = pdf_specifics(doc.context(), doc.document());
        QVERIFY(pdfDoc);
        if (!pdf_js_supported(doc.context(), pdfDoc))
            QSKIP("JavaScript is disabled in this MuPDF build");

        auto details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        auto button = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "IncrementButton";
        });
        QVERIFY(button != details.formFields.end());
        QCOMPARE(button->pushButtonAction, ::Mu::Model::FormPushButtonAction::JavaScript);

        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY2(doc.clickFormButton(0, button->pdfObjectNumber, &mutations, &error), error.c_str());

        details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto counter = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Counter";
        });
        QVERIFY(counter != details.formFields.end());
        QCOMPARE(counter->text, std::string("1"));

        const QString savedPath = dir.filePath(QStringLiteral("form_javascript_button-saved.pdf"));
        QFile savedFile(savedPath);
        QVERIFY(savedFile.open(QIODevice::WriteOnly));
        QVERIFY2(doc.saveFd(savedFile.handle(), &error), error.c_str());
        savedFile.close();

        QFile savedInput(savedPath);
        QVERIFY(savedInput.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument reopened;
        QVERIFY2(reopened.openFd(::dup(savedInput.handle()), "form_javascript_button-saved.pdf", &error),
                 error.c_str());
        const auto reopenedDetails = reopened.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto reopenedCounter = std::find_if(reopenedDetails.formFields.begin(),
                                                  reopenedDetails.formFields.end(),
                                                  [](const auto& field) { return field.partialName == "Counter"; });
        QVERIFY(reopenedCounter != reopenedDetails.formFields.end());
        QCOMPARE(reopenedCounter->text, std::string("1"));
    }

    void formJavaScriptFailuresStillReturnAppliedMutations()
    {
        QFile file(QStringLiteral(FORM_JS_PARTIAL_ERROR_PDF_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(file.handle()), "form_javascript_partial_error.pdf", &error), error.c_str());
        pdf_document* pdfDoc = pdf_specifics(doc.context(), doc.document());
        QVERIFY(pdfDoc);
        if (!pdf_js_supported(doc.context(), pdfDoc))
            QSKIP("JavaScript is disabled in this MuPDF build");

        const auto details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        const auto flag = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Flag";
        });
        const auto counter = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Counter";
        });
        QVERIFY(flag != details.formFields.end());
        QVERIFY(counter != details.formFields.end());

        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY2(
            doc.updateFormField(0, flag->pdfObjectNumber, ::Mu::Model::FormCheckValue { true }, &mutations, &error),
            error.c_str());
        const auto changedCounter = std::find_if(mutations.begin(), mutations.end(), [&](const auto& mutation) {
            return mutation.objectNumber == counter->pdfObjectNumber;
        });
        QVERIFY(changedCounter != mutations.end());
        const auto* changedText = std::get_if<::Mu::Model::FormTextValue>(&changedCounter->actualValue);
        QVERIFY(changedText);
        QCOMPARE(changedText->text, std::string("checkbox-up"));

        const auto updatedDetails = doc.pageDetails(0, &error);
        const auto button = std::find_if(updatedDetails.formFields.begin(),
                                         updatedDetails.formFields.end(),
                                         [](const auto& f) { return f.partialName == "ThrowingButton"; });
        QVERIFY(button != updatedDetails.formFields.end());
        QVERIFY2(doc.clickFormButton(0, button->pdfObjectNumber, &mutations, &error), error.c_str());
        const auto buttonCounter = std::find_if(mutations.begin(), mutations.end(), [&](const auto& mutation) {
            return mutation.objectNumber == counter->pdfObjectNumber;
        });
        QVERIFY(buttonCounter != mutations.end());
        const auto* buttonText = std::get_if<::Mu::Model::FormTextValue>(&buttonCounter->actualValue);
        QVERIFY(buttonText);
        QCOMPARE(buttonText->text, std::string("button-down"));
    }

    void formJavaScriptCheckboxAndRadioEventsRun()
    {
        QFile file(QStringLiteral(FORM_JS_CONTROLS_PDF_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(file.handle()), "form_javascript_controls.pdf", &error), error.c_str());

        pdf_document* pdfDoc = pdf_specifics(doc.context(), doc.document());
        QVERIFY(pdfDoc);
        if (!pdf_js_supported(doc.context(), pdfDoc))
            QSKIP("JavaScript is disabled in this MuPDF build");

        auto details = doc.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        auto checkbox = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.type == ::Mu::Model::FormFieldType::CheckBox;
        });
        auto radio = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.type == ::Mu::Model::FormFieldType::RadioButton && field.onState == "Second";
        });
        QVERIFY(checkbox != details.formFields.end());
        QVERIFY(radio != details.formFields.end());

        std::vector<::Mu::Worker::Engine::DocumentBase::FieldMutation> mutations;
        QVERIFY2(
            doc.updateFormField(0, checkbox->pdfObjectNumber, ::Mu::Model::FormCheckValue { true }, &mutations, &error),
            error.c_str());
        details = doc.pageDetails(0, &error);
        auto log = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Log";
        });
        QVERIFY(log != details.formFields.end());
        QCOMPARE(log->text, std::string("checkbox"));

        radio = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.type == ::Mu::Model::FormFieldType::RadioButton && field.onState == "Second";
        });
        QVERIFY(radio != details.formFields.end());
        QVERIFY2(
            doc.updateFormField(0, radio->pdfObjectNumber, ::Mu::Model::FormCheckValue { true }, &mutations, &error),
            error.c_str());
        details = doc.pageDetails(0, &error);
        log = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.partialName == "Log";
        });
        QVERIFY(log != details.formFields.end());
        QCOMPARE(log->text, std::string("radio"));

        const auto firstRadio =
            std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
                return field.type == ::Mu::Model::FormFieldType::RadioButton && field.onState == "First";
            });
        QVERIFY(firstRadio != details.formFields.end());
        QVERIFY(!firstRadio->checked);
        radio = std::find_if(details.formFields.begin(), details.formFields.end(), [](const auto& field) {
            return field.type == ::Mu::Model::FormFieldType::RadioButton && field.onState == "Second";
        });
        QVERIFY(radio->checked);
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

    void renderUsesPaperColor()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("paper.pdf"));
        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        createTextPDF(ctx, path);
        fz_drop_context(ctx);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(file.handle()), "paper.pdf", &error), error.c_str());

        const ::Mu::Worker::Engine::DocumentBase::RenderRequest request { 0, 300, 300, std::nullopt };
        std::vector<std::uint8_t> pixels(300U * 300U * 4U);

        // Default settings keep the white background.
        QVERIFY2(doc.renderToBuffer(request, pixels.data(), 300U * 4U, &error), error.c_str());
        QCOMPARE(pixels[0], 0xFF);
        QCOMPARE(pixels[3], 0xFF);

        // The Okular paper color replaces the background (last pixel = bottom
        // right corner, away from the text content).
        ::Mu::Model::DocumentSettings settings;
        settings.paperColorRgb = 0x112233;
        doc.setSettings(settings);
        QVERIFY2(doc.renderToBuffer(request, pixels.data(), 300U * 4U, &error), error.c_str());
        constexpr std::size_t lastPixel = (300U * 300U - 1U) * 4U;
        QCOMPARE(pixels[lastPixel], 0x11);
        QCOMPARE(pixels[lastPixel + 1], 0x22);
        QCOMPARE(pixels[lastPixel + 2], 0x33);
        QCOMPARE(pixels[lastPixel + 3], 0xFF);
    }

    void metadataReportsXfaForms_data()
    {
        QTest::addColumn<QByteArray>("acroForm");
        QTest::addColumn<bool>("expected");
        QTest::newRow("no-acroform") << QByteArray() << false;
        QTest::newRow("acroform-only") << QByteArray("/AcroForm << /Fields [] >>") << false;
        QTest::newRow("null-xfa") << QByteArray("/AcroForm << /Fields [] /XFA null >>") << false;
        QTest::newRow("xfa-stream") << QByteArray("/AcroForm << /Fields [] /XFA 5 0 R >>") << true;
        QTest::newRow("xfa-packets") << QByteArray("/AcroForm << /Fields [] /XFA [(template) 5 0 R] >>") << true;
        QTest::newRow("indirect-acroform") << QByteArray("/AcroForm 6 0 R") << true;
    }

    void metadataReportsXfaForms()
    {
        QFETCH(QByteArray, acroForm);
        QFETCH(bool, expected);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // A minimal, valid PDF with an indirect XFA stream. Its XML is never
        // interpreted; the catalog entry alone determines the metadata flag.
        const QList<QByteArray> objects {
            "<< /Type /Catalog /Pages 2 0 R " + acroForm + " >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources <<>> /Contents 4 0 R >>",
            "<< /Length 0 >>\nstream\n\nendstream",
            "<< /Length 6 >>\nstream\n<xfa/>\nendstream",
            "<< /Fields [] /XFA 5 0 R >>"
        };
        QByteArray data("%PDF-1.7\n");
        QByteArray xref("0000000000 65535 f \n");
        for (qsizetype i = 0; i < objects.size(); ++i) {
            xref += QByteArray::number(data.size()).rightJustified(10, '0') + " 00000 n \n";
            data += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const auto xrefOffset = data.size();
        data += "xref\n0 7\n" + xref + "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n"
            + QByteArray::number(xrefOffset) + "\n%%EOF\n";

        QFile file(dir.filePath(QStringLiteral("xfa.pdf")));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QCOMPARE(file.write(data), data.size());
        QVERIFY(file.flush());
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(file.handle()), "xfa.pdf", &error), error.c_str());
        const auto info = doc.metadata({ "hasXfaForm" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(info.values.at("hasXfaForm"), std::string(expected ? "true" : "false"));
        QCOMPARE(info.values.size(), size_t(1));
        QVERIFY(!doc.metadata({ "title" }, &error).values.contains("hasXfaForm"));

        // Reusing the document for a plain PDF must not retain XFA state.
        const QString plainPath = dir.filePath(QStringLiteral("plain.pdf"));
        createTextPDF(doc.context(), plainPath);
        QFile plain(plainPath);
        QVERIFY(plain.open(QIODevice::ReadOnly));
        QVERIFY2(doc.openFd(::dup(plain.handle()), "plain.pdf", &error), error.c_str());
        QCOMPARE(doc.metadata({ "hasXfaForm" }, &error).values.at("hasXfaForm"), std::string("false"));
        QVERIFY2(error.empty(), error.c_str());
    }

    void metadataReportsXrefRepairState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // 1. An intact document reports no repair.
        const QString intactPath = dir.filePath(QStringLiteral("intact.pdf"));
        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        createTextPDF(ctx, intactPath);
        fz_drop_context(ctx);
        QFile intactFile(intactPath);
        QVERIFY(intactFile.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument intact;
        QVERIFY2(intact.openFd(::dup(intactFile.handle()), "intact.pdf", &error), error.c_str());
        const auto intactMeta = intact.metadata({ "repaired" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(intactMeta.values.at("repaired"), std::string("false"));

        // 2. A corrupted startxref offset forces MuPDF's silent repair path;
        // the document still opens and the repair state is reported.
        const QString brokenPath = dir.filePath(QStringLiteral("broken.pdf"));
        QFile::copy(intactPath, brokenPath);
        QFile brokenFile(brokenPath);
        QVERIFY(brokenFile.open(QIODevice::ReadWrite));
        QByteArray data = brokenFile.readAll();
        const qsizetype startxref = data.lastIndexOf("startxref");
        QVERIFY(startxref > 0);
        const qsizetype digitsStart = data.indexOf('\n', startxref) + 1;
        const qsizetype digitsEnd = data.indexOf('\n', digitsStart);
        QVERIFY(digitsEnd > digitsStart);
        for (qsizetype i = digitsStart; i < digitsEnd; ++i)
            data[i] = '9';
        brokenFile.seek(0);
        brokenFile.write(data);
        brokenFile.close();

        QFile reopenedFile(brokenPath);
        QVERIFY(reopenedFile.open(QIODevice::ReadOnly));
        ::Mu::Worker::Engine::PdfDocument broken;
        QVERIFY2(broken.openFd(::dup(reopenedFile.handle()), "broken.pdf", &error), error.c_str());
        QCOMPARE(broken.pageCount(), 1);
        const auto brokenMeta = broken.metadata({ "repaired" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(brokenMeta.values.at("repaired"), std::string("true"));
    }

    void metadataReportsDates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString infoPath = dir.filePath(QStringLiteral("info.pdf"));

        // Build a minimal PDF whose Info dictionary carries PDF date strings.
        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
        std::string buildError;
        fz_try(ctx)
        {
            pdf_document* pdfDoc = pdf_create_document(ctx);
            const char* emptyContent = "q Q\n";
            fz_buffer* contents =
                fz_new_buffer_from_copied_data(ctx, (const unsigned char*)emptyContent, std::strlen(emptyContent));
            pdf_obj* resources = pdf_new_dict(ctx, pdfDoc, 0);
            pdf_obj* pageObj = pdf_add_page(ctx, pdfDoc, fz_unit_rect, 0, resources, contents);
            pdf_insert_page(ctx, pdfDoc, -1, pageObj);
            pdf_drop_obj(ctx, pageObj);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);

            pdf_obj* info = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Info));
            if (!info) {
                info = pdf_add_new_dict(ctx, pdfDoc, 4);
                pdf_dict_put_drop(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Info), info);
            }
            pdf_dict_put_text_string(ctx, info, PDF_NAME(CreationDate), "D:20240101120000+01'00'");
            pdf_dict_put_text_string(ctx, info, PDF_NAME(ModDate), "D:20240203040506");
            pdf_save_document(ctx, pdfDoc, QFile::encodeName(infoPath).constData(), &pdf_default_write_options);
            pdf_drop_document(ctx, pdfDoc);
        }
        fz_catch(ctx)
        {
            buildError = fz_caught_message(ctx);
        }
        fz_drop_context(ctx);
        QVERIFY2(buildError.empty(), buildError.c_str());

        QFile infoFile(infoPath);
        QVERIFY(infoFile.open(QIODevice::ReadOnly));
        std::string error;
        ::Mu::Worker::Engine::PdfDocument doc;
        QVERIFY2(doc.openFd(::dup(infoFile.handle()), "info.pdf", &error), error.c_str());

        // Unfiltered query: date values are normalized to ISO 8601 UTC
        // instants; the engine version is reported via ping, not metadata.
        const auto all = doc.metadata({ }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(all.values.at("creationDate"), std::string("2024-01-01T11:00:00Z"));
        QCOMPARE(all.values.at("modificationDate"), std::string("2024-02-03T04:05:06Z"));
        QCOMPARE(all.values.count("engineVersion"), size_t(0));

        // A non-empty key list filters common values like every other key.
        const auto filtered = doc.metadata({ "creationDate" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(filtered.values.size(), size_t(1));
        QCOMPARE(filtered.values.count("modificationDate"), size_t(0));
    }
};

int runTestWorkerDocument(int argc, char** argv)
{
    TestDocument test;
    return QTest::qExec(&test, argc, argv);
}

#include "document.moc"
