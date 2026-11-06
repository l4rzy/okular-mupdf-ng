#include "engine/mupdf_helpers.hpp"
#include "engine/pdf/document.hpp"
#include "genpdf.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QTest>

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <string>
#include <unistd.h>

#ifndef TEST_SIGNATURE_PDF_DIR
#define TEST_SIGNATURE_PDF_DIR "."
#endif

namespace {

using namespace Mu::Model;
using Mu::Worker::Engine::PdfDocument;

bool openSignatureDocument(PdfDocument& document, QTemporaryFile& source, std::string& error, bool withField = true)
{
    if (!source.open())
        return false;
    const auto path = source.fileName();
    source.close();
    fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!context)
        return false;
    const auto cleanup = qScopeGuard([&] { fz_drop_context(context); });
    if (withField)
        createSignaturePDF(context, path);
    else
        createTextPDF(context, path, "q Q\n");
    return document.openFd(::open(QFile::encodeName(path).constData(), O_RDONLY), "signature.pdf", &error);
}

SignRequest signRequest()
{
    SignRequest request;
    request.page = 0;
    request.rectangle = { .1, .1, .5, .2 };
    request.certificateNickname = "test-signer";
    request.certificateSubjectCommonName = "Test Signer";
    request.appearance.elements = SignatureElementSimple;
    request.appearance.signingEpochSeconds = 1'700'000'000;
    request.appearance.signingDisplayDate = "Fixed test date";
    return request;
}

// These dictionaries exercise metadata parsing, not cryptographic validation.
pdf_obj* setSignatureValue(PdfDocument& document)
{
    auto* context = document.context();
    auto* pdf = pdf_specifics(context, document.document());
    auto* page = pdf_load_page(context, pdf, 0);
    const auto cleanup = qScopeGuard([&] { pdf_drop_page(context, page); });
    auto* widget = pdf_first_widget(context, page);
    if (!widget)
        return nullptr;
    auto* value = pdf_dict_put_dict(context, pdf_annot_obj(context, widget), PDF_NAME(V), 8);
    pdf_dict_put_name(context, value, PDF_NAME(Type), "Sig");
    return value;
}

bool openDocument(Mu::Worker::Engine::PdfDocument& document, QFile& source, const QString& path, std::string& error)
{
    if (!source.open(QIODevice::ReadOnly)) {
        error = source.errorString().toStdString();
        return false;
    }
    return document.openFd(::dup(source.handle()), path.toStdString(), &error);
}

QByteArray rawSignatureGap(QFile& source, const ::Mu::Model::SignatureField& field)
{
    if (field.byteRange.size() != 4)
        return { };
    const qint64 gapOffset = field.byteRange[0] + field.byteRange[1];
    const qint64 gapEnd = field.byteRange[2];
    if (gapOffset < 0 || gapEnd < gapOffset || !source.seek(gapOffset))
        return { };
    return source.read(gapEnd - gapOffset);
}

QByteArray cmsFromSignatureGap(const QByteArray& gap)
{
    if (gap.size() < 2 || gap.front() != '<' || gap.back() != '>')
        return { };
    return QByteArray::fromHex(gap.mid(1, gap.size() - 2));
}

} // namespace

class TestWorkerSignature : public QObject {
    Q_OBJECT

private slots:

    void rejectsExistingTargetsWithoutMutation_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<QString>("diagnostic");
        QTest::newRow("missing") << 0 << QStringLiteral("signature field no longer exists");
        QTest::newRow("text-field") << 1 << QStringLiteral("target is not a signature field");
        QTest::newRow("signed") << 2 << QStringLiteral("signature field is already signed");
        QTest::newRow("read-only") << 3 << QStringLiteral("signature field is read-only");
    }

    void rejectsExistingTargetsWithoutMutation()
    {
        QFETCH(int, scenario);
        QFETCH(QString, diagnostic);
        QTemporaryFile source;
        PdfDocument document;
        std::string error;
        QVERIFY2(openSignatureDocument(document, source, error), error.c_str());
        auto* context = document.context();
        auto* pdf = pdf_specifics(context, document.document());
        auto* page = pdf_load_page(context, pdf, 0);
        const auto dropPage = qScopeGuard([&] { pdf_drop_page(context, page); });
        auto* widget = pdf_first_widget(context, page);
        QVERIFY(widget);
        auto* field = pdf_annot_obj(context, widget);
        auto request = signRequest();
        request.existingFieldObjectNumber = pdf_to_num(context, field);
        if (scenario == 0)
            request.existingFieldObjectNumber = std::numeric_limits<std::int32_t>::max();
        else if (scenario == 1)
            pdf_dict_put_name(context, field, PDF_NAME(FT), "Tx");
        else if (scenario == 2) {
            auto* signature = pdf_dict_put_dict(context, field, PDF_NAME(V), 2);
            pdf_dict_put_name(context, signature, PDF_NAME(Type), "Sig");
            pdf_dict_put_text_string(context, signature, PDF_NAME(Name), "Existing signer");
        } else
            pdf_dict_put_int(context, field, PDF_NAME(Ff), PDF_FIELD_IS_READ_ONLY);
        const int originalFlags = pdf_field_flags(context, field);
        const bool originallySigned = pdf_signature_is_signed(context, pdf, field);
        const std::string originalType = pdf_to_name(context, pdf_dict_get(context, field, PDF_NAME(FT)));
        int callbacks = 0;
        const auto callback = [&](const std::array<std::uint8_t, 32>&, const std::string&) {
            ++callbacks;
            return Mu::Worker::Engine::CmsResult { };
        };
        QTemporaryFile output;
        QVERIFY(output.open());
        const int fd = ::dup(output.handle());
        QVERIFY(fd >= 0);
        SigningResult result = SigningResult::Success;
        QVERIFY(!document.signFd(request, callback, fd, &result, &error));
        QCOMPARE(QString::fromStdString(error), diagnostic);
        QCOMPARE(result, SigningResult::GenericError);
        QCOMPARE(callbacks, 0);
        errno = 0;
        QCOMPARE(::fcntl(fd, F_GETFD), -1);
        QCOMPARE(errno, EBADF);
        QCOMPARE(pdf_field_flags(context, field), originalFlags);
        QCOMPARE(bool(pdf_signature_is_signed(context, pdf, field)), originallySigned);
        QCOMPARE(std::string(pdf_to_name(context, pdf_dict_get(context, field, PDF_NAME(FT)))), originalType);
        if (originallySigned)
            QCOMPARE(std::string(pdf_to_text_string(
                         context, pdf_dict_get(context, pdf_dict_get(context, field, PDF_NAME(V)), PDF_NAME(Name)))),
                     std::string("Existing signer"));
    }

    void rejectsInvalidRequests_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::newRow("invalid-fd") << 0;
        QTest::newRow("negative-page") << 1;
        QTest::newRow("past-end") << 2;
        QTest::newRow("empty-nickname") << 3;
        QTest::newRow("read-only-output") << 4;
    }

    void rejectsInvalidRequests()
    {
        QFETCH(int, scenario);
        QTemporaryFile source;
        PdfDocument document;
        std::string error;
        QVERIFY2(openSignatureDocument(document, source, error), error.c_str());
        auto request = signRequest();
        if (scenario == 1)
            request.page = -1;
        else if (scenario == 2)
            request.page = document.pageCount();
        else if (scenario == 3)
            request.certificateNickname.clear();
        QTemporaryFile output;
        QVERIFY(output.open());
        const int fd = scenario == 0 ? -1
            : scenario == 4          ? ::open(QFile::encodeName(output.fileName()).constData(), O_RDONLY)
                                     : ::dup(output.handle());
        if (scenario != 0)
            QVERIFY(fd >= 0);
        int callbacks = 0;
        const auto callback = [&](const std::array<std::uint8_t, 32>&, const std::string&) {
            ++callbacks;
            return Mu::Worker::Engine::CmsResult { };
        };
        SigningResult result = SigningResult::Success;
        QVERIFY(!document.signFd(request, callback, fd, &result, &error));
        QCOMPARE(error, std::string(scenario == 4 ? "could not adopt output FD" : "sign request is invalid"));
        QCOMPARE(result, SigningResult::GenericError);
        QCOMPARE(callbacks, 0);
        if (fd >= 0) {
            errno = 0;
            QCOMPARE(::fcntl(fd, F_GETFD), -1);
            QCOMPARE(errno, EBADF);
        }
        QCOMPARE(output.size(), 0);
        error.clear();
        const auto fields = document.pageDetails(0, &error).signatures;
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(fields.size(), std::size_t(1));
        QVERIFY(!fields.front().signedField);
    }

    void extractsHashAlgorithm_data()
    {
        QTest::addColumn<QString>("subFilter");
        QTest::addColumn<HashAlgorithm>("algorithm");
        QTest::newRow("sha256") << QStringLiteral("test.sha256") << HashAlgorithm::Sha256;
        QTest::newRow("sha384") << QStringLiteral("test.sha384") << HashAlgorithm::Sha384;
        QTest::newRow("sha512") << QStringLiteral("test.sha512") << HashAlgorithm::Sha512;
        QTest::newRow("sha1") << QStringLiteral("adbe.pkcs7.sha1") << HashAlgorithm::Sha1;
        QTest::newRow("md5") << QStringLiteral("test.md5") << HashAlgorithm::Md5;
        QTest::newRow("unknown") << QStringLiteral("adbe.pkcs7.detached") << HashAlgorithm::Unknown;
        QTest::newRow("missing") << QString() << HashAlgorithm::Unknown;
    }

    void extractsHashAlgorithm()
    {
        QFETCH(QString, subFilter);
        QFETCH(HashAlgorithm, algorithm);
        QTemporaryFile source;
        PdfDocument document;
        std::string error;
        QVERIFY2(openSignatureDocument(document, source, error), error.c_str());
        auto* context = document.context();
        auto* signature = setSignatureValue(document);
        QVERIFY(signature);
        if (!subFilter.isEmpty())
            pdf_dict_put_name(context, signature, PDF_NAME(SubFilter), subFilter.toUtf8().constData());
        // Non-positive PDF dates must not become apparently valid signing times.
        pdf_dict_put_text_string(context, signature, PDF_NAME(M), "D:19600101000000Z");
        const auto fields = document.pageDetails(0, &error).signatures;
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(fields.size(), std::size_t(1));
        const auto& field = fields.front();
        QVERIFY(field.signedField);
        QCOMPARE(field.hashAlgorithm, algorithm);
        QCOMPARE(field.subFilter, subFilter.toStdString());
        QVERIFY(field.signerName.empty());
        QVERIFY(field.reason.empty());
        QVERIFY(field.location.empty());
        QVERIFY(!field.signingTime.valid);
        QVERIFY(field.cmsSignature.empty());
        QCOMPARE(field.signatureStatus, SignatureStatus::NotVerified);
    }

    void validatesByteRanges_data()
    {
        QTest::addColumn<int>("scenario");
        const char* names[] { "complete",      "partial",
                              "nonzero-start", "negative-first-length",
                              "overlap",       "negative-second-length",
                              "past-eof",      "overflow-length",
                              "short-array",   "not-array",
                              "missing" };
        for (int i = 0; i < int(std::size(names)); ++i)
            QTest::newRow(names[i]) << i;
    }

    void validatesByteRanges()
    {
        QFETCH(int, scenario);
        QTemporaryFile source;
        PdfDocument document;
        std::string error;
        QVERIFY2(openSignatureDocument(document, source, error), error.c_str());
        auto* context = document.context();
        auto* signature = setSignatureValue(document);
        QVERIFY(signature);
        const std::int64_t fileSize = QFileInfo(source.fileName()).size();
        QVERIFY(fileSize > 20);
        std::array<std::int64_t, 4> range { 0, 10, 20, fileSize - 20 };
        switch (scenario) {
        case 1:
            range[3] = 0;
            break;
        case 2:
            range[0] = 1;
            break;
        case 3:
            range[1] = -1;
            break;
        case 4:
            range[1] = 21;
            break;
        case 5:
            range[3] = -1;
            break;
        case 6:
            range[2] = fileSize + 1;
            range[3] = 0;
            break;
        case 7:
            range[3] = std::numeric_limits<std::int64_t>::max();
            break;
        default:
            break;
        }
        if (scenario < 9) {
            auto* array = pdf_dict_put_array(context, signature, PDF_NAME(ByteRange), 4);
            for (int i = 0; i < (scenario == 8 ? 3 : 4); ++i)
                pdf_array_push_int(context, array, range[static_cast<std::size_t>(i)]);
        } else if (scenario == 9)
            pdf_dict_put_int(context, signature, PDF_NAME(ByteRange), 0);
        const auto fields = document.pageDetails(0, &error).signatures;
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(fields.size(), std::size_t(1));
        QCOMPARE(fields.front().signsTotalDocument, scenario == 0);
        if (scenario < 8)
            QCOMPARE(fields.front().byteRange, std::vector<std::int64_t>(range.begin(), range.end()));
        else
            QVERIFY(fields.front().byteRange.empty());
    }

    void enforcesSignedMetadataBudget_data()
    {
        QTest::addColumn<int>("extraBytes");
        QTest::newRow("byte-range") << 31;
        QTest::newRow("signer-name") << 35;
        QTest::newRow("reason") << 41;
        QTest::newRow("location") << 49;
        QTest::newRow("sub-filter") << 68;
    }

    void enforcesSignedMetadataBudget()
    {
        QFETCH(int, extraBytes);
        QTemporaryFile source;
        PdfDocument document;
        std::string error;
        QVERIFY2(openSignatureDocument(document, source, error), error.c_str());
        const auto original = document.pageDetails(0, &error).signatures;
        QCOMPARE(original.size(), std::size_t(1));
        auto* context = document.context();
        auto* signature = setSignatureValue(document);
        QVERIFY(signature);
        pdf_dict_put_text_string(context, signature, PDF_NAME(Name), "Name");
        pdf_dict_put_text_string(context, signature, PDF_NAME(Reason), "Reason");
        pdf_dict_put_text_string(context, signature, PDF_NAME(Location), "Location");
        pdf_dict_put_name(context, signature, PDF_NAME(SubFilter), "adbe.pkcs7.detached");
        auto* ranges = pdf_dict_put_array(context, signature, PDF_NAME(ByteRange), 4);
        for (int i = 0; i < 4; ++i)
            pdf_array_push_int(context, ranges, 0);
        Mu::Worker::Engine::ExtractionBudgets budgets;
        budgets.metadata.remainingBytes = sizeof(SignatureField) + original.front().partialName.size()
            + original.front().fullyQualifiedName.size() + static_cast<std::size_t>(extraBytes);
        const auto* exceptionTop = context->error.top;
        QVERIFY(document.pageDetails(0, &error, false, &budgets).signatures.empty());
        QCOMPARE(error, std::string(Mu::Worker::Engine::ExtractionBudgetError));
        QCOMPARE(context->error.top, exceptionTop);
        error.clear();
        const auto restored = document.pageDetails(0, &error).signatures;
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(restored.size(), std::size_t(1));
        QCOMPARE(restored.front().signerName, std::string("Name"));
        QCOMPARE(restored.front().reason, std::string("Reason"));
        QCOMPARE(restored.front().location, std::string("Location"));
    }

    void writesSignatureAppearance_data()
    {
        QTest::addColumn<quint8>("elements");
        QTest::addColumn<bool>("border");
        QTest::newRow("name-only") << quint8(SignatureElement::GraphicName) << false;
        QTest::newRow("name-and-info") << quint8(SignatureElementDefault) << false;
        QTest::newRow("border-only") << quint8(0) << true;
    }

    void writesSignatureAppearance()
    {
        QFETCH(quint8, elements);
        QFETCH(bool, border);
        QTemporaryFile source;
        PdfDocument document;
        std::string error;
        QVERIFY2(openSignatureDocument(document, source, error, false), error.c_str());
        auto request = signRequest();
        request.appearance.elements = elements;
        request.appearance.drawBorder = border;
        int callbacks = 0;
        const auto callback = [&](const std::array<std::uint8_t, 32>&, const std::string& nickname) {
            ++callbacks;
            // A deterministic CMS placeholder tests worker serialization and
            // appearance only. Real cryptography remains in the plugin suite.
            return Mu::Worker::Engine::CmsResult { SigningResult::Success, nickname, { 0x30, 0x00 } };
        };
        QTemporaryFile output;
        QVERIFY(output.open());
        SigningResult result = SigningResult::GenericError;
        QVERIFY2(document.signFd(request, callback, ::dup(output.handle()), &result, &error), error.c_str());
        QCOMPARE(result, SigningResult::Success);
        QCOMPARE(callbacks, 1);
        QFile signedSource(output.fileName());
        PdfDocument signedDocument;
        QVERIFY2(openDocument(signedDocument, signedSource, output.fileName(), error), error.c_str());
        const auto fields = signedDocument.pageDetails(0, &error).signatures;
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(fields.size(), std::size_t(1));
        QVERIFY(fields.front().signedField);
        QVERIFY(fields.front().signsTotalDocument);
        QCOMPARE(fields.front().signingTime.unixMilliseconds, std::int64_t(1'700'000'000'000));
        QCOMPARE(fields.front().signatureStatus, SignatureStatus::NotVerified);
        QImage image(612, 792, QImage::Format_RGBA8888);
        QVERIFY2(signedDocument.renderToBuffer({ 0, image.width(), image.height(), std::nullopt },
                                               image.bits(),
                                               static_cast<std::size_t>(image.bytesPerLine()),
                                               &error),
                 error.c_str());
        bool hasInk = false;
        for (int y = 79; y < 159 && !hasInk; ++y)
            for (int x = 61; x < 306 && !hasInk; ++x) {
                const auto color = image.pixelColor(x, y);
                hasInk = color.red() < 240 || color.green() < 240 || color.blue() < 240;
            }
        QVERIFY(hasInk);
        QVERIFY(document.pageDetails(0, &error).signatures.empty());
        QVERIFY(error.empty());
    }

    void extractsSignatureData()
    {
        const QString path = QStringLiteral(TEST_SIGNATURE_PDF_DIR) + QStringLiteral("/digital_signature.pdf");
        QFile source(path);
        Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(openDocument(document, source, path, error), error.c_str());

        bool foundSignedField = false;
        for (int pageNumber = 0; pageNumber < document.pageCount(); ++pageNumber) {
            error.clear();
            const auto details = document.pageDetails(pageNumber, &error);
            QVERIFY2(error.empty(), error.c_str());
            QCOMPARE(details.signatures.size(), size_t(1));
            for (const auto& field : details.signatures) {
                if (!field.signedField)
                    continue;
                foundSignedField = true;
                QCOMPARE(field.partialName, std::string("Signature2"));
                QCOMPARE(field.signerName, std::string("John B Harris"));
                QCOMPARE(field.reason, std::string("I am the author of this document"));
                QCOMPARE(field.subFilter, std::string("adbe.pkcs7.detached"));
                QCOMPARE(field.signingTime.unixMilliseconds, std::int64_t(1'247'755'667'000));
                QCOMPARE(field.byteRange.size(), size_t(4));
                QCOMPARE(field.byteRange[0], std::int64_t(0));
                QCOMPARE(field.byteRange[1], std::int64_t(227'012));
                QCOMPARE(field.byteRange[2], std::int64_t(248'956));
                QCOMPARE(field.byteRange[3], std::int64_t(23'362));
                QVERIFY(field.signsTotalDocument);

                const QByteArray rawGap = rawSignatureGap(source, field);
                const QByteArray expectedCms = cmsFromSignatureGap(rawGap);
                const QByteArray actualCms(reinterpret_cast<const char*>(field.cmsSignature.data()),
                                           static_cast<qsizetype>(field.cmsSignature.size()));
                QCOMPARE(rawGap.size(), 21'944);
                QCOMPARE(expectedCms.size(), 10'971);
                QCOMPARE(actualCms, expectedCms);
                QCOMPARE(QCryptographicHash::hash(actualCms, QCryptographicHash::Sha256),
                         QByteArray::fromHex("d9333a3b75edff14e4a1e9f1ca1c0ef78c603a957b495fbac787c07b3f807ae0"));
            }
        }
        QVERIFY2(foundSignedField, "worker found no signed signature field");
    }

    void formatsSignatureDate()
    {
        // Pin the timezone so expectations are deterministic.
        const QByteArray previousTimeZone = qgetenv("TZ");
        ::setenv("TZ", "America/Chicago", 1);
        ::tzset();
        const auto restoreTimeZone = qScopeGuard([&] {
            if (previousTimeZone.isEmpty())
                ::unsetenv("TZ");
            else
                ::setenv("TZ", previousTimeZone.constData(), 1);
            ::tzset();
        });

        using Mu::Worker::Engine::formatSignatureDate;
        // TZ=America/Chicago: 2026-09-07 13:14 CDT.
        QCOMPARE(QString::fromStdString(formatSignatureDate(1'788'804'840)), QStringLiteral("Sep 7, 2026 13:14 CDT"));
        // Epoch 0: two-digit day must not be stripped.
        QCOMPARE(QString::fromStdString(formatSignatureDate(0)), QStringLiteral("Dec 31, 1969 18:00 CST"));
    }

    void failingCmsCleansUpAppearanceState_data()
    {
        QTest::addColumn<bool>("customDisplayDate");
        QTest::addColumn<bool>("newField");
        QTest::newRow("existing-generated-date") << false << false;
        QTest::newRow("existing-supplied-date") << true << false;
        QTest::newRow("new-generated-date") << false << true;
        QTest::newRow("new-supplied-date") << true << true;
    }

    void failingCmsCleansUpAppearanceState()
    {
        QFETCH(bool, customDisplayDate);
        QFETCH(bool, newField);
        // A CMS callback failure throws after the worker has built the
        // signature appearance text; the owning strings must be destroyed on
        // that longjmp path (LeakSanitizer catches a regression here).
        QTemporaryFile sourceFile;
        QVERIFY(sourceFile.open());
        const QString sourcePath = sourceFile.fileName();
        sourceFile.close();
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createSignaturePDF(context, sourcePath);
        fz_drop_context(context);

        QFile source(sourcePath);
        Mu::Worker::Engine::PdfDocument document;
        std::string error;
        QVERIFY2(openDocument(document, source, sourcePath, error), error.c_str());

        const auto details = document.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.signatures.size(), size_t(1));
        QVERIFY(!details.signatures.front().signedField);

        const auto failingCms = [](const std::array<std::uint8_t, 32>&, const std::string&) {
            return ::Mu::Worker::Engine::CmsResult { ::Mu::Model::SigningResult::GenericError,
                                                     "rejected by test",
                                                     { } };
        };

        QTemporaryFile output;
        QVERIFY(output.open());
        const int outputFd = ::dup(output.handle());
        QVERIFY(outputFd >= 0);
        ::Mu::Model::SigningResult signingResult;
        const bool signedPdf = document.signFd(
            ::Mu::Model::SignRequest {
                .file = { },
                .page = 0,
                .rectangle = { .1, .1, .5, .2 },
                .certificateNickname = "okular-mupdf-test",
                .certificateSubjectCommonName = "Okular MuPDF Test Signer",
                .existingFieldObjectNumber = newField ? -1 : details.signatures.front().objectNumber,
                .appearance = { .elements = ::Mu::Model::SignatureElementSimple,
                                .reason = "Rejected approval",
                                .location = "Winnipeg",
                                .signingDisplayDate = customDisplayDate ? std::string(128, 'x') : std::string(),
                                .backgroundImage = { } },
            },
            failingCms,
            outputFd,
            &signingResult,
            &error);
        QVERIFY2(!signedPdf, "signing must fail when the CMS callback is rejected");
        errno = 0;
        QCOMPARE(::fcntl(outputFd, F_GETFD), -1);
        QCOMPARE(errno, EBADF);
        QCOMPARE(signingResult, ::Mu::Model::SigningResult::GenericError);
        QVERIFY(!error.empty());
        error.clear();
        const auto restored = document.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(restored.signatures.size(), size_t(1));
        QVERIFY(!restored.signatures.front().signedField);
        QVERIFY(restored.signatures.front().reason.empty());
        QVERIFY(restored.signatures.front().location.empty());
    }
};

int runTestWorkerSignature(int argc, char** argv)
{
    TestWorkerSignature test;
    return QTest::qExec(&test, argc, argv);
}

#include "signature.moc"
