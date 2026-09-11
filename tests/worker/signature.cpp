#include "engine/mupdf_helpers.hpp"
#include "engine/pdf/document.hpp"
#include "genpdf.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QTest>

#include <array>
#include <cstdint>
#include <string>
#include <unistd.h>

#ifndef TEST_SIGNATURE_PDF_DIR
#define TEST_SIGNATURE_PDF_DIR "."
#endif

namespace {

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

    void failingCmsCleansUpAppearanceState()
    {
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
        ::Mu::Model::SigningResult signingResult;
        const bool signedPdf = document.signFd(
            ::Mu::Model::SignRequest {
                .file = { },
                .page = 0,
                .rectangle = { .1, .1, .5, .2 },
                .certificateNickname = "okular-mupdf-test",
                .certificateSubjectCommonName = "Okular MuPDF Test Signer",
                .existingFieldObjectNumber = details.signatures.front().objectNumber,
                .appearance = { },
            },
            failingCms,
            ::dup(output.handle()),
            &signingResult,
            &error);
        QVERIFY2(!signedPdf, "signing must fail when the CMS callback is rejected");
        QCOMPARE(signingResult, ::Mu::Model::SigningResult::GenericError);
        QVERIFY(!error.empty());
    }
};

int runTestWorkerSignature(int argc, char** argv)
{
    TestWorkerSignature test;
    return QTest::qExec(&test, argc, argv);
}

#include "signature.moc"
