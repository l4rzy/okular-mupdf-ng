#include "engine/pdf/document.hpp"
#include "generator/config/certmanager/dialog_utils.hpp"
#include "generator/proxy/certificate_store.hpp"
#include "generator/proxy/form/signature.hpp"
#include "genpdf.hpp"
#include "plugin/crypto/certificate_database.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/nss_internal.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <algorithm>
#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <certdb.h>
#pragma pop_macro("slots")
#include <cstring>
#include <secoid.h>
#include <unistd.h>

#ifndef TEST_SIGNATURE_PDF_DIR
#define TEST_SIGNATURE_PDF_DIR "."
#endif

namespace {

bool openDocument(Mu::Worker::Engine::PdfDocument& document, QFile& source, const QString& path)
{
    if (!source.open(QIODevice::ReadOnly))
        return false;
    return document.openFd(::dup(source.handle()), path.toStdString());
}

Mu::Worker::Engine::CmsResult createCms(const std::array<std::uint8_t, 32>& digest, const std::string& nickname)
{
    const auto result =
        ::Mu::Plugin::Crypto::createDetachedCmsFromDigest(QString::fromStdString(nickname), { }, digest);
    return { result.result,
             result.details.toStdString(),
             std::vector<std::uint8_t>(result.cms.cbegin(), result.cms.cend()) };
}

} // namespace

class TestSignatureValidation : public QObject {
    Q_OBJECT

    QTemporaryDir m_nssDb;

    bool runCertutil(const QStringList& arguments, const QByteArray& input = { })
    {
        QProcess process;
        process.start(QStringLiteral("certutil"), arguments);
        if (!process.waitForStarted()) {
            qWarning("certutil could not start: %s", qPrintable(process.errorString()));
            return false;
        }
        if (!input.isEmpty()) {
            process.write(input);
            process.closeWriteChannel();
        }
        const bool success =
            process.waitForFinished() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!success)
            qWarning("certutil failed: %s", qPrintable(process.readAllStandardError().trimmed()));
        return success;
    }

    bool runPk12util(const QStringList& arguments)
    {
        QProcess process;
        process.start(QStringLiteral("pk12util"), arguments);
        if (!process.waitForStarted()) {
            qWarning("pk12util could not start: %s", qPrintable(process.errorString()));
            return false;
        }
        const bool success =
            process.waitForFinished() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!success)
            qWarning("pk12util failed: %s", qPrintable(process.readAllStandardError().trimmed()));
        return success;
    }

    bool runOpenSsl(const QStringList& arguments)
    {
        QProcess process;
        process.start(QStringLiteral("openssl"), arguments);
        if (!process.waitForStarted()) {
            qWarning("openssl could not start: %s", qPrintable(process.errorString()));
            return false;
        }
        const bool success =
            process.waitForFinished() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!success)
            qWarning("openssl failed: %s", qPrintable(process.readAllStandardError().trimmed()));
        return success;
    }

private slots:

    void mapsNssDigestAlgorithms()
    {
        const QList<std::pair<SECOidTag, ::Mu::Model::HashAlgorithm>> cases {
            { SEC_OID_MD5, ::Mu::Model::HashAlgorithm::Md5 },
            { SEC_OID_SHA1, ::Mu::Model::HashAlgorithm::Sha1 },
            { SEC_OID_SHA224, ::Mu::Model::HashAlgorithm::Sha224 },
            { SEC_OID_SHA256, ::Mu::Model::HashAlgorithm::Sha256 },
            { SEC_OID_SHA384, ::Mu::Model::HashAlgorithm::Sha384 },
            { SEC_OID_SHA512, ::Mu::Model::HashAlgorithm::Sha512 },
            { SEC_OID_UNKNOWN, ::Mu::Model::HashAlgorithm::Unknown },
        };
        for (const auto& test : cases)
            QCOMPARE(::Mu::Plugin::Crypto::Internal::hashAlgorithmForDigest(test.first), test.second);
    }

    void resetsDerivedStateForMalformedSignature()
    {
        QBuffer source;
        source.setData(QByteArrayLiteral("document"));
        QVERIFY(source.open(QIODevice::ReadOnly));

        ::Mu::Model::SignatureField field;
        field.signedField = true;
        field.cmsSignature = { 0x30, 0x01, 0x00 };
        field.byteRange = { 0, 1, 1, 0 };
        field.hashAlgorithm = ::Mu::Model::HashAlgorithm::Sha512;
        field.signerName = "stale signer";
        field.signerSubjectDn = "stale subject";
        field.certificate.null = false;
        field.certificateStatus = ::Mu::Model::CertificateStatus::Trusted;

        ::Mu::Plugin::Crypto::validateDetachedPdfSignature(field, source);

        QCOMPARE(field.signatureStatus, ::Mu::Model::SignatureStatus::DecodingError);
        QCOMPARE(field.hashAlgorithm, ::Mu::Model::HashAlgorithm::Unknown);
        QVERIFY(field.certificate.null);
        QVERIFY(field.signerName.empty());
        QVERIFY(field.signerSubjectDn.empty());
        QCOMPARE(field.certificateStatus, ::Mu::Model::CertificateStatus::NotVerified);
    }

    void reportsInvalidByteRangeAsDecodingError_data()
    {
        QTest::addColumn<QList<qint64>>("byteRange");
        QTest::newRow("negative firstLength") << QList<qint64> { 0, -1, 1, 0 };
        QTest::newRow("firstOffset not zero") << QList<qint64> { 1, 1, 2, 0 };
        QTest::newRow("secondOffset before firstLength") << QList<qint64> { 0, 2, 1, 1 };
        QTest::newRow("negative secondLength") << QList<qint64> { 0, 1, 1, -1 };
        QTest::newRow("secondOffset beyond size") << QList<qint64> { 0, 1, 100, 1 };
        QTest::newRow("secondLength overflow") << QList<qint64> { 0, 5, 5, 10 };
    }

    void reportsInvalidByteRangeAsDecodingError()
    {
        QFETCH(QList<qint64>, byteRange);
        QBuffer source;
        source.setData(QByteArrayLiteral("document"));
        QVERIFY(source.open(QIODevice::ReadOnly));

        ::Mu::Model::SignatureField field;
        field.signedField = true;
        field.cmsSignature = { 0x30, 0x01, 0x00 };
        field.byteRange.assign(byteRange.cbegin(), byteRange.cend());

        ::Mu::Plugin::Crypto::validateDetachedPdfSignature(field, source);

        QCOMPARE(field.signatureStatus, ::Mu::Model::SignatureStatus::DecodingError);
        QCOMPARE(field.hashAlgorithm, ::Mu::Model::HashAlgorithm::Unknown);
    }

    void initTestCase()
    {
        QVERIFY2(m_nssDb.isValid(), "could not create temporary NSS database");
        QFile noise(m_nssDb.filePath(QStringLiteral("noise")));
        QVERIFY(noise.open(QIODevice::WriteOnly));
        QVERIFY(noise.write(QByteArray(4096, '\x5a')) == 4096);
        noise.close();

        const QString database = QStringLiteral("sql:") + m_nssDb.path();
        QVERIFY2(
            runCertutil({ QStringLiteral("-N"), QStringLiteral("-d"), database, QStringLiteral("--empty-password") }),
            "certutil could not create an NSS database");
        QVERIFY2(runCertutil({ QStringLiteral("-S"),
                               QStringLiteral("-x"),
                               QStringLiteral("-n"),
                               QStringLiteral("okular-mupdf-test-root"),
                               QStringLiteral("-s"),
                               QStringLiteral("CN=Okular MuPDF Test Root"),
                               QStringLiteral("-t"),
                               QStringLiteral(",CT,"),
                               QStringLiteral("-k"),
                               QStringLiteral("rsa"),
                               QStringLiteral("-g"),
                               QStringLiteral("2048"),
                               QStringLiteral("-m"),
                               QStringLiteral("1"),
                               QStringLiteral("-v"),
                               QStringLiteral("12"),
                               QStringLiteral("-Z"),
                               QStringLiteral("SHA256"),
                               QStringLiteral("-2"),
                               QStringLiteral("-d"),
                               database,
                               QStringLiteral("-z"),
                               noise.fileName() },
                             "y\n\nn\n"),
                 "certutil could not create the signing root certificate");
        QVERIFY2(runCertutil({ QStringLiteral("-S"),
                               QStringLiteral("-n"),
                               QStringLiteral("okular-mupdf-test"),
                               QStringLiteral("-s"),
                               QStringLiteral("CN=Okular MuPDF Test Signer"),
                               QStringLiteral("-c"),
                               QStringLiteral("okular-mupdf-test-root"),
                               QStringLiteral("-t"),
                               QStringLiteral(",u,"),
                               QStringLiteral("-k"),
                               QStringLiteral("rsa"),
                               QStringLiteral("-g"),
                               QStringLiteral("2048"),
                               QStringLiteral("-m"),
                               QStringLiteral("2"),
                               QStringLiteral("-v"),
                               QStringLiteral("12"),
                               QStringLiteral("-Z"),
                               QStringLiteral("SHA256"),
                               QStringLiteral("-1"),
                               QStringLiteral("--keyUsage"),
                               QStringLiteral("digitalSignature"),
                               QStringLiteral("-6"),
                               QStringLiteral("--extKeyUsage"),
                               QStringLiteral("emailProtection"),
                               QStringLiteral("-d"),
                               database,
                               QStringLiteral("-z"),
                               noise.fileName() }),
                 "certutil could not create the signing certificate");
        QVERIFY2(::Mu::Plugin::Crypto::initializeNss(m_nssDb.path()) == ::Mu::Plugin::Crypto::NssRuntimeMode::ReadWrite,
                 "NSS certificate database could not be initialized");
        QCOMPARE(::Mu::Plugin::Crypto::activeNssDatabasePath(), m_nssDb.path());
    }

    void signedPdf_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<int>("expectedPluginSignature");
        QTest::addColumn<int>("expectedPluginCertificate");
        QTest::addColumn<int>("expectedProxySignature");
        QTest::addColumn<int>("expectedProxyCertificate");
        QTest::addColumn<bool>("expectedSignsTotalDocument");
        QTest::newRow("digital signature")
            << QStringLiteral("digital_signature.pdf") << static_cast<int>(::Mu::Model::SignatureStatus::Valid)
            << static_cast<int>(::Mu::Model::CertificateStatus::Expired)
            << static_cast<int>(Okular::SignatureInfo::SignatureValid)
            << static_cast<int>(Okular::SignatureInfo::CertificateExpired) << true;
        QTest::newRow("digital signature 2") << QStringLiteral("digital_signature2.pdf")
                                             << static_cast<int>(::Mu::Model::SignatureStatus::DigestMismatch)
                                             << static_cast<int>(::Mu::Model::CertificateStatus::Expired)
                                             << static_cast<int>(Okular::SignatureInfo::SignatureDigestMismatch)
                                             << static_cast<int>(Okular::SignatureInfo::CertificateExpired) << false;
    }

    void passwordlessCertificateDoesNotPrompt()
    {
        ::Mu::Generator::Proxy::CertificateStore store;
        const auto certificates = store.signingCertificates(nullptr);
        const auto certificate =
            std::find_if(certificates.cbegin(), certificates.cend(), [](const Okular::CertificateInfo& value) {
                return value.nickName() == QStringLiteral("okular-mupdf-test");
            });
        QVERIFY(certificate != certificates.cend());
        QVERIFY(certificate->checkPassword({ }));
    }

    void signingCertificateLookupResults()
    {
        const QString nickname = QStringLiteral("okular-mupdf-test");
        QVERIFY(!::Mu::Plugin::Crypto::checkSigningCertificatePassword(QStringLiteral("missing"), { }));
        QVERIFY(::Mu::Plugin::Crypto::checkSigningCertificatePassword(nickname, { }));
        QVERIFY(!::Mu::Plugin::Crypto::checkSigningCertificatePassword(nickname, QStringLiteral("wrong")));
        QCOMPARE(::Mu::Plugin::Crypto::signingCertificateCommonName(nickname),
                 QStringLiteral("Okular MuPDF Test Signer"));
        QCOMPARE(::Mu::Plugin::Crypto::signingCertificateCommonName(QStringLiteral("missing")), QString());
    }

    void signingDoesNotModifyDigest()
    {
        std::array<std::uint8_t, 32> digest { };
        std::fill(digest.begin(), digest.end(), 0x5a);
        const auto original = digest;
        const auto result =
            ::Mu::Plugin::Crypto::createDetachedCmsFromDigest(QStringLiteral("okular-mupdf-test"), { }, digest);
        QCOMPARE(result.result, ::Mu::Model::SigningResult::Success);
        QCOMPARE(digest, original);
        QVERIFY(!result.cms.isEmpty());
    }

    void createsSelfSignedCertificate()
    {
        ::Mu::Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions options;
        options.nickname = QStringLiteral("okular-mupdf-generated");
        options.commonName = QStringLiteral("Generated, Signing + Certificate");
        options.organization = QStringLiteral("Okular \"MuPDF\"");
        options.country = QStringLiteral("CA");
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addYears(1);

        QString error;
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error),
            qPrintable(error));

        const auto certificates = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto certificate = std::find_if(certificates.cbegin(), certificates.cend(), [](const auto& value) {
            return value.nickname == "okular-mupdf-generated";
        });
        QVERIFY(certificate != certificates.cend());
        QCOMPARE(certificate->subjectCommonName, std::string("Generated, Signing + Certificate"));
        QCOMPARE(certificate->issuerDistinguishedName, certificate->subjectDistinguishedName);

        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(
                     m_nssDb.path(), QStringLiteral("okular-mupdf-generated"), &error),
                 qPrintable(error));
    }

    void validatesSelfSignedCertificateWithoutChainRecursion()
    {
        const QString nickname = QStringLiteral("okular-mupdf-self-signed-validation");
        ::Mu::Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions options;
        options.nickname = nickname;
        options.commonName = QStringLiteral("Self-Signed Validation Test");
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addYears(1);

        QString error;
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error),
            qPrintable(error));

        const QByteArray document = QByteArrayLiteral("self-signed validation document");
        QBuffer source;
        source.setData(document);
        QVERIFY(source.open(QIODevice::ReadOnly));

        const QByteArray digestBytes = QCryptographicHash::hash(document, QCryptographicHash::Sha256);
        std::array<std::uint8_t, 32> digest { };
        std::memcpy(digest.data(), digestBytes.constData(), digest.size());
        const auto cms = createCms(digest, nickname.toStdString());
        QCOMPARE(cms.result, ::Mu::Model::SigningResult::Success);

        ::Mu::Model::SignatureField field;
        field.signedField = true;
        field.cmsSignature = cms.cmsSignature;
        field.byteRange = { 0, document.size(), document.size(), 0 };
        ::Mu::Plugin::Crypto::validateDetachedPdfSignature(field, source);

        QCOMPARE(field.signatureStatus, ::Mu::Model::SignatureStatus::Valid);
        QCOMPARE(field.certificateStatus, ::Mu::Model::CertificateStatus::UntrustedIssuer);

        CERTCertDBHandle* certdb = CERT_GetDefaultCertDB();
        QVERIFY(certdb);
        const QByteArray nicknameBytes = nickname.toUtf8();
        CERTCertificate* certificate = CERT_FindCertByNickname(certdb, nicknameBytes.constData());
        QVERIFY(certificate);
        CERTCertTrust trust { };
        QVERIFY(CERT_GetCertTrust(certificate, &trust) == SECSuccess);
        trust.emailFlags |= CERTDB_TRUSTED;
        QVERIFY(CERT_ChangeCertTrust(certdb, certificate, &trust) == SECSuccess);
        CERT_DestroyCertificate(certificate);

        auto trustedField = field;
        ::Mu::Plugin::Crypto::validateDetachedPdfSignature(trustedField, source);
        QCOMPARE(trustedField.signatureStatus, ::Mu::Model::SignatureStatus::Valid);
        QCOMPARE(trustedField.certificateStatus, ::Mu::Model::CertificateStatus::Trusted);

        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), nickname, &error),
                 qPrintable(error));
    }

    void importsPkcs12WithCorrectPassword()
    {
        const QString database = QStringLiteral("sql:") + m_nssDb.path();
        const QString sourceNickname = QStringLiteral("okular-mupdf-pkcs12-source");
        ::Mu::Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions options;
        options.nickname = sourceNickname;
        options.commonName = QStringLiteral("PKCS#12 Import Test");
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addYears(1);
        QString error;
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error),
            qPrintable(error));

        QTemporaryFile bundle;
        QVERIFY(bundle.open());
        const QString bundlePath = bundle.fileName();
        bundle.close();
        QVERIFY(QFile::remove(bundlePath));

        QVERIFY2(runPk12util({ QStringLiteral("-o"),
                               bundlePath,
                               QStringLiteral("-n"),
                               sourceNickname,
                               QStringLiteral("-d"),
                               database,
                               QStringLiteral("-K"),
                               QString(),
                               QStringLiteral("-W"),
                               QStringLiteral("päss-密码") }),
                 "pk12util could not export the test bundle");
        QFile bundleFile(bundlePath);
        QVERIFY(bundleFile.open(QIODevice::ReadOnly));
        const QByteArray data = bundleFile.readAll();
        bundleFile.close();
        QVERIFY(!data.isEmpty());

        QVERIFY2(
            runCertutil({ QStringLiteral("-D"), QStringLiteral("-n"), sourceNickname, QStringLiteral("-d"), database }),
            "certutil could not remove the source certificate");

        QVERIFY2(!::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(
                     m_nssDb.path(), data, QStringLiteral("wrong-password"), &error),
                 "an incorrect PKCS#12 password was accepted");
        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(
                     m_nssDb.path(), data, QStringLiteral("päss-密码"), &error),
                 qPrintable(error));

        const auto certificates = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto certificate = std::find_if(certificates.cbegin(), certificates.cend(), [](const auto& value) {
            return value.nickname == "okular-mupdf-pkcs12-source";
        });
        QVERIFY(certificate != certificates.cend());

        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), sourceNickname, &error),
                 qPrintable(error));
    }

    void rejectsInactiveCertificateDatabase()
    {
        QTemporaryDir otherDatabase;
        QVERIFY(otherDatabase.isValid());

        QString error;
        QVERIFY(!::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(
            otherDatabase.path(), QByteArrayLiteral("not a PKCS#12 bundle"), { }, &error));
        QVERIFY2(error.contains(QStringLiteral("not active")), qPrintable(error));
    }

    void importsModernOpenSslPkcs12()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
            QSKIP("openssl is required for the modern PKCS#12 interoperability test");
        QTemporaryDir source;
        QVERIFY(source.isValid());
        const QString keyPath = source.filePath(QStringLiteral("key.pem"));
        const QString certificatePath = source.filePath(QStringLiteral("certificate.pem"));
        const QString bundlePath = source.filePath(QStringLiteral("certificate.p12"));
        QVERIFY2(runOpenSsl({ QStringLiteral("req"),
                              QStringLiteral("-x509"),
                              QStringLiteral("-newkey"),
                              QStringLiteral("rsa:2048"),
                              QStringLiteral("-nodes"),
                              QStringLiteral("-keyout"),
                              keyPath,
                              QStringLiteral("-out"),
                              certificatePath,
                              QStringLiteral("-subj"),
                              QStringLiteral("/CN=Modern PKCS12 Test"),
                              QStringLiteral("-days"),
                              QStringLiteral("1") }),
                 "openssl could not create a test certificate");
        QVERIFY2(runOpenSsl({ QStringLiteral("pkcs12"),
                              QStringLiteral("-export"),
                              QStringLiteral("-inkey"),
                              keyPath,
                              QStringLiteral("-in"),
                              certificatePath,
                              QStringLiteral("-out"),
                              bundlePath,
                              QStringLiteral("-name"),
                              QStringLiteral("modern-pkcs12-test"),
                              QStringLiteral("-keypbe"),
                              QStringLiteral("AES-256-CBC"),
                              QStringLiteral("-certpbe"),
                              QStringLiteral("AES-256-CBC"),
                              QStringLiteral("-macalg"),
                              QStringLiteral("SHA256"),
                              QStringLiteral("-passout"),
                              QStringLiteral("pass:pass") }),
                 "openssl could not create a modern PKCS#12 bundle");

        QFile bundle(bundlePath);
        QVERIFY(bundle.open(QIODevice::ReadOnly));
        const QByteArray data = bundle.readAll();
        QVERIFY(!data.isEmpty());

        QString error;
        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(
                     m_nssDb.path(), data, QStringLiteral("pass"), &error),
                 qPrintable(error));
        const auto certificates = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto certificate = std::find_if(certificates.cbegin(), certificates.cend(), [](const auto& value) {
            return value.nickname == "modern-pkcs12-test";
        });
        QVERIFY(certificate != certificates.cend());
        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(
                     m_nssDb.path(), QStringLiteral("modern-pkcs12-test"), &error),
                 qPrintable(error));
    }

    void publicOnlyCertificateImportRollback()
    {
        const QString database = QStringLiteral("sql:") + m_nssDb.path();
        QTemporaryFile certFile;
        QVERIFY(certFile.open());
        const QString certPath = certFile.fileName();
        certFile.close();

        // Export the root public certificate as DER
        QVERIFY2(runCertutil({ QStringLiteral("-L"),
                               QStringLiteral("-n"),
                               QStringLiteral("okular-mupdf-test-root"),
                               QStringLiteral("-d"),
                               database,
                               QStringLiteral("-r"),
                               QStringLiteral("-o"),
                               certPath }),
                 "certutil could not export public root certificate");

        QFile certReader(certPath);
        QVERIFY(certReader.open(QIODevice::ReadOnly));
        const QByteArray certDer = certReader.readAll();
        certReader.close();
        QVERIFY(!certDer.isEmpty());

        QString error;
        // Attempt to import public-only certificate as a signing cert
        const bool imported = ::Mu::Plugin::Crypto::CertificateDatabase::importCertificate(
            m_nssDb.path(), certDer, QStringLiteral("orphan-public-cert"), &error);
        QVERIFY(!imported);
        QVERIFY2(error.contains(QStringLiteral("has no associated private key")), qPrintable(error));

        // Verify it was rolled back and is not listed as a signing cert
        const auto signingCerts = ::Mu::Plugin::Crypto::signingCertificates();
        const auto it = std::find_if(signingCerts.cbegin(), signingCerts.cend(), [](const auto& cert) {
            return cert.nickname == "orphan-public-cert";
        });
        QVERIFY(it == signingCerts.cend());
    }

    void rejectsInvalidSelfSignedCertificateInput()
    {
        using ::Mu::Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions;

        SelfSignedCertificateOptions options;
        options.commonName = QStringLiteral("Missing nickname");
        options.validFrom = QDateTime::currentDateTime();
        options.validUntil = options.validFrom.addYears(1);
        QString error;
        QVERIFY(
            !::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error));
        QVERIFY(!error.isEmpty());

        options.nickname = QStringLiteral("invalid-country");
        options.country = QStringLiteral("CAN");
        error.clear();
        QVERIFY(
            !::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error));
        QVERIFY(!error.isEmpty());

        options.country = QStringLiteral("CA");
        options.validUntil = options.validFrom;
        error.clear();
        QVERIFY(
            !::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error));
        QVERIFY(!error.isEmpty());
    }

    void formatsCertificateDatabaseDialogPath()
    {
        using ::Mu::Generator::CertificateManager::displayDatabasePath;

        QCOMPARE(displayDatabasePath(QStringLiteral("short-path")), QStringLiteral("short-path"));
        const QString exactLength(30, QLatin1Char('x'));
        QCOMPARE(displayDatabasePath(exactLength), exactLength);
        const QString longPath(31, QLatin1Char('x'));
        const QString compactPath = displayDatabasePath(longPath);
        QCOMPARE(compactPath.size(), 30);
        QVERIFY(compactPath.endsWith(QStringLiteral("...")));
        QCOMPARE(displayDatabasePath({ }), QStringLiteral("Default NSS database"));
    }

    void signedPdf()
    {
        QFETCH(QString, fileName);
        QFETCH(int, expectedPluginSignature);
        QFETCH(int, expectedPluginCertificate);
        QFETCH(int, expectedProxySignature);
        QFETCH(int, expectedProxyCertificate);
        QFETCH(bool, expectedSignsTotalDocument);
        const QString path = QStringLiteral(TEST_SIGNATURE_PDF_DIR) + QLatin1Char('/') + fileName;
        QFile source(path);
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY2(openDocument(document, source, path), qPrintable(path));

        bool foundSignature = false;
        for (int pageNumber = 0; pageNumber < document.pageCount(); ++pageNumber) {
            const auto details = document.pageDetails(pageNumber);
            for (const auto& workerField : details.signatures) {
                if (!workerField.signedField)
                    continue;
                foundSignature = true;
                QVERIFY2(!workerField.cmsSignature.empty(), "worker did not extract PDF /Contents");
                QCOMPARE(workerField.byteRange.size(), size_t(4));
                QVERIFY(workerField.cmsSignature.front() == 0x30);

                auto field = workerField;
                ::Mu::Plugin::Crypto::validateDetachedPdfSignature(field, source);
                QVERIFY2(!field.certificate.null, "plugin did not extract the CMS signer certificate");
                QVERIFY2(!field.certificate.der.empty(), "certificate DER is missing");
                QVERIFY2(!field.signerSubjectDn.empty(), "certificate subject DN is missing");
                QCOMPARE(field.partialName, std::string("Signature2"));
                QCOMPARE(field.signerName, std::string("John B Harris"));
                QCOMPARE(
                    field.signerSubjectDn,
                    std::string(
                        "E=jbharris@adobe.com,CN=John B Harris,O=Adobe Systems Incorporated,L=San Jose,ST=CA,C=US"));
                QCOMPARE(field.hashAlgorithm, ::Mu::Model::HashAlgorithm::Sha1);
                auto repeatedField = workerField;
                ::Mu::Plugin::Crypto::validateDetachedPdfSignature(repeatedField, source);
                QCOMPARE(repeatedField.signatureStatus, field.signatureStatus);
                QCOMPARE(repeatedField.hashAlgorithm, field.hashAlgorithm);
                if (expectedPluginSignature == static_cast<int>(::Mu::Model::SignatureStatus::Valid)
                    && field.signatureStatus == ::Mu::Model::SignatureStatus::Invalid)
                    QSKIP("NSS rejected the legacy SHA-1 signature under the active system crypto policy");
                QCOMPARE(field.signatureStatus, static_cast<::Mu::Model::SignatureStatus>(expectedPluginSignature));
                QCOMPARE(field.certificateStatus,
                         static_cast<::Mu::Model::CertificateStatus>(expectedPluginCertificate));
                if (expectedSignsTotalDocument) {
                    QVERIFY(field.signingTime.valid);
                    QVERIFY(field.signsTotalDocument);
                } else {
                    QVERIFY(!field.signsTotalDocument);
                }

                ::Mu::Generator::Proxy::Form::Signature proxy(
                    1, field, static_cast<::Mu::Plugin::WorkerClient*>(nullptr));
                const Okular::SignatureInfo info = proxy.signatureInfo();
                QCOMPARE(proxy.signatureType(), Okular::FormFieldSignature::AdbePkcs7detached);
                QCOMPARE(info.signerName(), QStringLiteral("John B Harris"));
                QCOMPARE(
                    info.signerSubjectDN(),
                    QStringLiteral(
                        "E=jbharris@adobe.com,CN=John B Harris,O=Adobe Systems Incorporated,L=San Jose,ST=CA,C=US"));
                QCOMPARE(info.hashAlgorithm(), Okular::SignatureInfo::HashAlgorithmSha1);
                QVERIFY(!info.certificateInfo().isNull());
                QVERIFY(!info.certificateInfo().certificateData().isEmpty());
                QCOMPARE(info.signatureStatus(),
                         static_cast<Okular::SignatureInfo::SignatureStatus>(expectedProxySignature));
                QCOMPARE(info.certificateStatus(),
                         static_cast<Okular::SignatureInfo::CertificateStatus>(expectedProxyCertificate));
                if (expectedSignsTotalDocument) {
                    QVERIFY(info.signingTime().isValid());
                    QVERIFY(info.signsTotalDocument());
                } else {
                    QVERIFY(!info.signsTotalDocument());
                }
            }
        }
        QVERIFY2(foundSignature, "PDF contains no signed signature field");
    }

    void writesPdfWithTrustedEmailSigningChain()
    {
        const QString sourcePath = QStringLiteral(TEST_SIGNATURE_PDF_DIR) + QStringLiteral("/pdfreference1.0.pdf");
        QFile source(sourcePath);
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY2(openDocument(document, source, sourcePath), qPrintable(sourcePath));

        QTemporaryFile output;
        QVERIFY(output.open());
        const int outputFd = ::dup(output.handle());
        QVERIFY(outputFd >= 0);
        std::string error;
        ::Mu::Model::SigningResult signingResult;
        const bool signedPdf = document.signFd(
            ::Mu::Model::SignRequest {
                .file = { },
                .page = 0,
                .rectangle = { .1, .1, .5, .2 },
                .certificateNickname = "okular-mupdf-test",
                .certificateSubjectCommonName = "Okular MuPDF Test Signer",
                .reason = "Okular signing test",
                .location = "Test location",
                .existingFieldObjectNumber = -1,
                .backgroundImage = { },
            },
            createCms,
            outputFd,
            &signingResult,
            &error);
        QVERIFY2(signedPdf, error.c_str());
        QVERIFY(output.flush());

        QFile signedSource(output.fileName());
        Mu::Worker::Engine::PdfDocument signedDocument;
        QVERIFY2(openDocument(signedDocument, signedSource, output.fileName()), qPrintable(output.fileName()));
        const auto details = signedDocument.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(!details.signatures.empty());

        bool foundSignature = false;
        for (auto field : details.signatures) {
            if (!field.signedField)
                continue;
            foundSignature = true;
            QVERIFY(field.partialName.starts_with("OkularMuPDFSignature"));
            QVERIFY(!field.cmsSignature.empty());
            QVERIFY(field.byteRange[0] == 0);
            QVERIFY(field.byteRange[1] >= 0);
            QVERIFY(field.byteRange[2] >= field.byteRange[1]);
            QVERIFY(field.byteRange[3] >= 0);
            QCOMPARE(field.byteRange[2] + field.byteRange[3], signedSource.size());
            QVERIFY(field.signsTotalDocument);

            ::Mu::Plugin::Crypto::validateDetachedPdfSignature(field, signedSource);
            QCOMPARE(field.signatureStatus, ::Mu::Model::SignatureStatus::Valid);
            QVERIFY(field.certificate.subjectDistinguishedName != field.certificate.issuerDistinguishedName);
            QCOMPARE(field.certificateStatusCurrent, ::Mu::Model::CertificateStatus::Trusted);
            QCOMPARE(field.certificateStatus, ::Mu::Model::CertificateStatus::Trusted);
            QCOMPARE(field.hashAlgorithm, ::Mu::Model::HashAlgorithm::Sha256);
            QVERIFY(field.signsTotalDocument);
        }
        QVERIFY(foundSignature);
    }

    void newSignatureDoesNotLockExistingFormFields()
    {
        QTemporaryFile sourceFile;
        QVERIFY(sourceFile.open());
        const QString sourcePath = sourceFile.fileName();
        sourceFile.close();
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createEditableTextFieldPDF(context, sourcePath);
        fz_drop_context(context);

        QFile source(sourcePath);
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY2(openDocument(document, source, sourcePath), qPrintable(sourcePath));
        std::string error;
        const auto initialDetails = document.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(initialDetails.formFields.size(), size_t(1));
        const std::int32_t formObjectNumber = initialDetails.formFields.front().pdfObjectNumber;

        QTemporaryFile output;
        QVERIFY(output.open());
        QVERIFY2(document.signFd(
                     ::Mu::Model::SignRequest {
                         .file = { },
                         .page = 0,
                         .rectangle = { .1, .1, .5, .2 },
                         .certificateNickname = "okular-mupdf-test",
                         .certificateSubjectCommonName = "Okular MuPDF Test Signer",
                         .reason = { },
                         .location = { },
                         .existingFieldObjectNumber = -1,
                         .backgroundImage = { },
                     },
                     createCms,
                     ::dup(output.handle()),
                     nullptr,
                     &error),
                 error.c_str());
        QVERIFY(output.flush());

        QFile signedSource(output.fileName());
        Mu::Worker::Engine::PdfDocument signedDocument;
        QVERIFY2(openDocument(signedDocument, signedSource, output.fileName()), qPrintable(output.fileName()));
        const auto details = signedDocument.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.formFields.size(), size_t(1));
        QCOMPARE(details.formFields.front().partialName, std::string("EditableField"));
        QCOMPARE(details.formFields.front().pdfObjectNumber, formObjectNumber);
        QVERIFY(!details.formFields.front().readOnly);
    }

    void signsExistingField()
    {
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
        QVERIFY2(openDocument(document, source, sourcePath), qPrintable(sourcePath));
        const auto unsignedFields = document.pageDetails(0).signatures;
        QCOMPARE(unsignedFields.size(), size_t(1));
        QVERIFY(!unsignedFields.front().signedField);
        QVERIFY(unsignedFields.front().objectNumber >= 0);
        std::string error;
        const auto unsignedMetadata = document.metadata({ "signatureCount" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(unsignedMetadata.values.at("signatureCount"), std::string("0"));

        QTemporaryFile output;
        QVERIFY(output.open());
        QVERIFY2(document.signFd(
                     ::Mu::Model::SignRequest {
                         .file = { },
                         .page = 0,
                         .rectangle = { },
                         .certificateNickname = "okular-mupdf-test",
                         .certificateSubjectCommonName = "Okular MuPDF Test Signer",
                         .reason = { },
                         .location = { },
                         .existingFieldObjectNumber = unsignedFields.front().objectNumber,
                         .backgroundImage = { },
                     },
                     createCms,
                     ::dup(output.handle()),
                     nullptr,
                     &error),
                 error.c_str());
        QVERIFY(output.flush());

        QFile signedSource(output.fileName());
        Mu::Worker::Engine::PdfDocument signedDocument;
        QVERIFY(openDocument(signedDocument, signedSource, output.fileName()));
        const auto signedFields = signedDocument.pageDetails(0).signatures;
        QCOMPARE(signedFields.size(), size_t(1));
        QVERIFY(signedFields.front().signedField);
        QCOMPARE(signedFields.front().partialName, std::string("Approval"));
        const auto signedMetadata = signedDocument.metadata({ "signatureCount" }, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(signedMetadata.values.at("signatureCount"), std::string("1"));
    }

    void signsDocumentWithBackgroundImage()
    {
        const QString sourcePath = QStringLiteral(TEST_SIGNATURE_PDF_DIR) + QStringLiteral("/pdfreference1.0.pdf");
        QFile source(sourcePath);
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY2(openDocument(document, source, sourcePath), qPrintable(sourcePath));

        // Generate sample background PNG image (red circle stamp on transparent canvas)
        QImage stamp(100, 100, QImage::Format_ARGB32);
        stamp.fill(Qt::transparent);
        QPainter painter(&stamp);
        painter.setBrush(QColor(200, 50, 50, 180));
        painter.drawEllipse(10, 10, 80, 80);
        painter.end();

        QByteArray pngBytes;
        QBuffer buffer(&pngBytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(stamp.save(&buffer, "PNG"));

        const std::vector<std::uint8_t> bgImage(reinterpret_cast<const std::uint8_t*>(pngBytes.constData()),
                                                reinterpret_cast<const std::uint8_t*>(pngBytes.constData())
                                                    + pngBytes.size());

        QTemporaryFile output;
        QVERIFY(output.open());
        const int outputFd = ::dup(output.handle());
        QVERIFY(outputFd >= 0);
        std::string error;
        ::Mu::Model::SigningResult signingResult;
        const bool signedPdf = document.signFd(
            ::Mu::Model::SignRequest {
                .file = { },
                .page = 0,
                .rectangle = { .1, .1, .5, .3 },
                .certificateNickname = "okular-mupdf-test",
                .certificateSubjectCommonName = "Okular MuPDF Test Signer",
                .reason = "Visual background test",
                .location = "Test location",
                .existingFieldObjectNumber = -1,
                .backgroundImage = bgImage,
            },
            createCms,
            outputFd,
            &signingResult,
            &error);
        QVERIFY2(signedPdf, error.c_str());
        QVERIFY(output.flush());

        QFile signedSource(output.fileName());
        Mu::Worker::Engine::PdfDocument signedDocument;
        QVERIFY2(openDocument(signedDocument, signedSource, output.fileName()), qPrintable(output.fileName()));
        const auto details = signedDocument.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.signatures.size(), size_t(1));
        QVERIFY(details.signatures.front().signedField);
        QVERIFY(details.signatures.front().partialName.starts_with("OkularMuPDFSignature"));
        QVERIFY(!details.signatures.front().cmsSignature.empty());
        QVERIFY(details.signatures.front().signsTotalDocument);
    }

    void signsDocumentWithCorruptBackgroundImage()
    {
        const QString sourcePath = QStringLiteral(TEST_SIGNATURE_PDF_DIR) + QStringLiteral("/pdfreference1.0.pdf");
        QFile source(sourcePath);
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY2(openDocument(document, source, sourcePath), qPrintable(sourcePath));

        // Malformed image payload: should gracefully fall back to standard appearance
        const std::vector<std::uint8_t> corruptImage { 0xFF, 0xD8, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF };

        QTemporaryFile output;
        QVERIFY(output.open());
        const int outputFd = ::dup(output.handle());
        QVERIFY(outputFd >= 0);
        std::string error;
        ::Mu::Model::SigningResult signingResult;
        const bool signedPdf = document.signFd(
            ::Mu::Model::SignRequest {
                .file = { },
                .page = 0,
                .rectangle = { .1, .1, .5, .3 },
                .certificateNickname = "okular-mupdf-test",
                .certificateSubjectCommonName = "Okular MuPDF Test Signer",
                .reason = "Corrupt background fallback test",
                .location = "Test location",
                .existingFieldObjectNumber = -1,
                .backgroundImage = corruptImage,
            },
            createCms,
            outputFd,
            &signingResult,
            &error);
        QVERIFY2(signedPdf, error.c_str());
        QVERIFY(output.flush());

        QFile signedSource(output.fileName());
        Mu::Worker::Engine::PdfDocument signedDocument;
        QVERIFY2(openDocument(signedDocument, signedSource, output.fileName()), qPrintable(output.fileName()));
        const auto details = signedDocument.pageDetails(0, &error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(details.signatures.size(), size_t(1));
        QVERIFY(details.signatures.front().signedField);
    }
};

int runTestPluginSignature(int argc, char** argv)
{
    TestSignatureValidation test;
    return QTest::qExec(&test, argc, argv);
}

#include "validation.moc"
