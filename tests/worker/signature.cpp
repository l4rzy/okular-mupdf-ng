#include "engine/pdf/document.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QTest>

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
};

int runTestWorkerSignature(int argc, char** argv)
{
    TestWorkerSignature test;
    return QTest::qExec(&test, argc, argv);
}

#include "signature.moc"
