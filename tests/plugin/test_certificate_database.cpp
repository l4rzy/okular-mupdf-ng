// SPDX-License-Identifier: GPL-3.0-or-later
#include "generator/config/certmanager/dialog_utils.hpp"
#include "plugin/crypto/certificate_database.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/nss_handles.hpp"
#include "plugin/worker_client.hpp"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <certdb.h>
#include <p12.h>
#include <pk11pub.h>
#include <prerror.h>
#include <secerr.h>
#pragma pop_macro("slots")

namespace {

thread_local bool failPkcs12ImportAfterWrite = false;

}

extern "C" SECStatus __real_SEC_PKCS12DecoderImportBags(SEC_PKCS12DecoderContext* decoder);

extern "C" SECStatus __wrap_SEC_PKCS12DecoderImportBags(SEC_PKCS12DecoderContext* decoder)
{
    const SECStatus result = __real_SEC_PKCS12DecoderImportBags(decoder);
    if (result == SECSuccess && std::exchange(failPkcs12ImportAfterWrite, false)) {
        PR_SetError(SEC_ERROR_IO, 0);
        return SECFailure;
    }
    return result;
}

namespace {

using namespace paperbox::nss;

auto findManagedCertificate(const QList<::Mu::Plugin::Crypto::CertificateDatabase::CertificateRecord>& certificates,
                            const std::string& nickname)
{
    return std::find_if(certificates.cbegin(), certificates.cend(), [&](const auto& record) {
        return record.certificate.nickname == nickname;
    });
}

struct CertificateDatabaseSnapshot {
    QMap<QByteArray, QByteArray> certificates;
    QSet<QByteArray> privateKeys;
};

// Snapshot every persistent slot certificate, including keyless certificates,
// plus key identities. Counts alone would miss replacement or trust changes.
std::optional<CertificateDatabaseSnapshot> snapshotCertificateDatabase()
{
    using namespace ::Mu::Plugin::Crypto;
    std::lock_guard lock(paperbox::nss::nssMutex());
    NssSlot slot(PK11_GetInternalKeySlot());
    if (!slot)
        return std::nullopt;
    NssCertificateList certificates(PK11_ListCertsInSlot(slot.get()));
    if (!certificates)
        return std::nullopt;
    CertificateDatabaseSnapshot result;
    for (auto* node = CERT_LIST_HEAD(certificates.get()); !CERT_LIST_END(node, certificates.get());
         node = CERT_LIST_NEXT(node)) {
        CERTCertTrust trust { };
        if (CERT_GetCertTrust(node->cert, &trust) != SECSuccess)
            return std::nullopt;
        const QByteArray der(reinterpret_cast<const char*>(node->cert->derCert.data), node->cert->derCert.len);
        QByteArray state;
        QDataStream stream(&state, QIODevice::WriteOnly);
        stream << der << QByteArray(node->cert->nickname ? node->cert->nickname : "") << quint32(trust.sslFlags)
               << quint32(trust.emailFlags) << quint32(trust.objectSigningFlags);
        result.certificates.insert(QCryptographicHash::hash(der, QCryptographicHash::Sha256), state);
    }
    std::unique_ptr<SECKEYPrivateKeyList, decltype(&SECKEY_DestroyPrivateKeyList)> keys(
        PK11_ListPrivateKeysInSlot(slot.get()), SECKEY_DestroyPrivateKeyList);
    if (!keys)
        return std::nullopt;
    for (auto* node = PRIVKEY_LIST_HEAD(keys.get()); !PRIVKEY_LIST_END(node, keys.get());
         node = PRIVKEY_LIST_NEXT(node)) {
        SECItem* identity = PK11_GetLowLevelKeyIDForPrivateKey(node->key);
        if (!identity)
            return std::nullopt;
        const auto freeIdentity = qScopeGuard([identity] { SECITEM_FreeItem(identity, PR_TRUE); });
        result.privateKeys.insert(QByteArray(reinterpret_cast<const char*>(identity->data), identity->len));
    }
    return result;
}

// A failed assertion must not leave newly imported identities in the shared
// suite database. Existing fingerprints are never cleanup targets.
void removeNewSigningCertificates(const QString& databasePath, const CertificateDatabaseSnapshot& before)
{
    using namespace ::Mu::Plugin::Crypto::CertificateDatabase;
    QString error;
    const auto records = listCertificates(databasePath, &error);
    if (!error.isEmpty()) {
        qWarning("Could not inspect import fixture during cleanup: %s", qPrintable(error));
        return;
    }
    for (const auto& record : records) {
        if (!before.certificates.contains(record.identity.sha256Fingerprint)
            && !deleteCertificate(databasePath, record.identity, &error))
            qWarning("Could not remove imported test certificate: %s", qPrintable(error));
    }
}

} // namespace

using namespace paperbox::nss;

class TestCertificateDatabase : public QObject {
    Q_OBJECT

    QTemporaryDir m_nssDb;

    bool runCertutil(const QStringList& arguments, const QByteArray& input = { })
    {
        QProcess process;
        process.start(QStringLiteral("certutil"), arguments);
        if (!process.waitForStarted(10000)) {
            qWarning("certutil could not start: %s", qPrintable(process.errorString()));
            process.kill();
            return false;
        }
        if (!input.isEmpty()) {
            process.write(input);
            process.closeWriteChannel();
        }
        const bool success =
            process.waitForFinished(60000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!success) {
            qWarning("certutil failed: %s", qPrintable(process.readAllStandardError().trimmed()));
            process.kill();
        }
        return success;
    }

    bool runPk12util(const QStringList& arguments)
    {
        QProcess process;
        process.start(QStringLiteral("pk12util"), arguments);
        if (!process.waitForStarted(10000)) {
            qWarning("pk12util could not start: %s", qPrintable(process.errorString()));
            process.kill();
            return false;
        }
        const bool success =
            process.waitForFinished(60000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!success) {
            qWarning("pk12util failed: %s", qPrintable(process.readAllStandardError().trimmed()));
            process.kill();
        }
        return success;
    }

    bool runOpenSsl(const QStringList& arguments)
    {
        QProcess process;
        process.start(QStringLiteral("openssl"), arguments);
        if (!process.waitForStarted(10000)) {
            qWarning("openssl could not start: %s", qPrintable(process.errorString()));
            process.kill();
            return false;
        }
        const bool success =
            process.waitForFinished(60000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!success) {
            qWarning("openssl failed: %s", qPrintable(process.readAllStandardError().trimmed()));
            process.kill();
        }
        return success;
    }

    std::optional<QByteArray> createPkcs12Bundle(const QString& nickname, const QString& commonName)
    {
        QTemporaryDir source;
        if (!source.isValid())
            return std::nullopt;
        const QString key = source.filePath(QStringLiteral("key.pem"));
        const QString certificate = source.filePath(QStringLiteral("certificate.pem"));
        const QString path = source.filePath(QStringLiteral("certificate.p12"));
        if (!runOpenSsl({ "req",
                          "-x509",
                          "-newkey",
                          "ec",
                          "-pkeyopt",
                          "ec_paramgen_curve:P-256",
                          "-nodes",
                          "-keyout",
                          key,
                          "-out",
                          certificate,
                          "-subj",
                          "/CN=" + commonName,
                          "-days",
                          "1",
                          "-sha256" })
            || !runOpenSsl({ "pkcs12",
                             "-export",
                             "-inkey",
                             key,
                             "-in",
                             certificate,
                             "-out",
                             path,
                             "-name",
                             nickname,
                             "-passout",
                             "pass:password" }))
            return std::nullopt;
        QFile bundle(path);
        if (!bundle.open(QIODevice::ReadOnly))
            return std::nullopt;
        return bundle.readAll();
    }

private slots:

    void initTestCase()
    {
        QVERIFY2(m_nssDb.isValid(), "could not create temporary NSS database");
        if (QStandardPaths::findExecutable(QStringLiteral("certutil")).isEmpty())
            QSKIP("certutil is required for the signature validation tests");
        if (QStandardPaths::findExecutable(QStringLiteral("pk12util")).isEmpty())
            QSKIP("pk12util is required for the signature validation tests");
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

    void createsSelfSignedCertificate_data()
    {
        using Mu::Plugin::Crypto::CertificateDatabase::SigningKey;
        QTest::addColumn<int>("signingKey");
        QTest::addColumn<int>("bits");
        QTest::newRow("RSA-2048") << static_cast<int>(SigningKey::Rsa2048) << 2048;
        QTest::newRow("RSA-3072") << static_cast<int>(SigningKey::Rsa3072) << 3072;
        QTest::newRow("RSA-4096") << static_cast<int>(SigningKey::Rsa4096) << 4096;
        QTest::newRow("ECDSA-P256") << static_cast<int>(SigningKey::EcdsaP256) << 256;
    }

    void createsSelfSignedCertificate()
    {
        using namespace Mu::Plugin::Crypto::CertificateDatabase;
        QFETCH(int, signingKey);
        QFETCH(int, bits);
        ::Mu::Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions options;
        options.signingKey = static_cast<SigningKey>(signingKey);
        options.nickname = QStringLiteral("okular-mupdf-generated-%1").arg(signingKey);
        options.commonName = QStringLiteral("Generated, Signing + Certificate");
        options.organization = QStringLiteral("Okular \"MuPDF\"");
        options.country = QStringLiteral("CA");
        options.email = QStringLiteral("signer+test@example.org");
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addYears(1);

        QString error;
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error),
            qPrintable(error));

        const auto certificates = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto certificate = findManagedCertificate(certificates, options.nickname.toStdString());
        QVERIFY(certificate != certificates.cend());
        QCOMPARE(certificate->certificate.subjectCommonName, std::string("Generated, Signing + Certificate"));
        QCOMPARE(certificate->certificate.subjectEmail, std::string("signer+test@example.org"));
        QCOMPARE(certificate->certificate.issuerDistinguishedName, certificate->certificate.subjectDistinguishedName);

        QCOMPARE(certificate->certificate.publicKeyStrength, bits);
        QCOMPARE(certificate->certificate.publicKeyType,
                 static_cast<std::int32_t>(bits == 256 ? Mu::Model::PublicKeyAlgorithm::Ec
                                                       : Mu::Model::PublicKeyAlgorithm::Rsa));
        QByteArray keyIdBytes;
        paperbox::nss::NssSlot signingSlot(PK11_GetInternalKeySlot());
        QVERIFY(signingSlot);
        {
            paperbox::nss::NssCertificate native(
                CERT_FindCertByNickname(CERT_GetDefaultCertDB(), options.nickname.toUtf8().constData()));
            QVERIFY(native);
            SECItem* keyId = PK11_GetLowLevelKeyIDForCert(signingSlot.get(), native.get(), nullptr);
            QVERIFY(keyId);
            keyIdBytes = QByteArray(reinterpret_cast<const char*>(keyId->data), static_cast<int>(keyId->len));
            SECITEM_FreeItem(keyId, PR_TRUE);
            QVERIFY(
                CERT_VerifySignedDataWithPublicKeyInfo(&native->signatureWrap, &native->subjectPublicKeyInfo, nullptr)
                == SECSuccess);
            QCOMPARE(SECOID_GetAlgorithmTag(&native->signatureWrap.signatureAlgorithm),
                     bits == 256 ? SEC_OID_ANSIX962_ECDSA_SHA256_SIGNATURE : SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION);
            if (bits == 256) {
                paperbox::nss::NssPublicKey key(CERT_ExtractPublicKey(native.get()));
                QVERIFY(key);
                QCOMPARE(QByteArray(reinterpret_cast<const char*>(key->u.ec.DEREncodedParams.data),
                                    static_cast<int>(key->u.ec.DEREncodedParams.len)),
                         QByteArray::fromHex("06082a8648ce3d030107"));
                QCOMPARE(native->signatureWrap.signatureAlgorithm.parameters.len, 0U);
            }
        }

        // Verify the generated identity through the complete PDF signing path.
        Mu::Plugin::WorkerClient backend;
        QVERIFY(backend.start(QStringLiteral(WORKER_BUILD_PATH)));
        const QString path = QStringLiteral(TEST_SIGNATURE_PDF_DIR "/pdfreference1.0.pdf");
        QList<Mu::Model::PageInfo> pages;
        QCOMPARE(backend.open(path, { }, pages), Mu::Model::OpenStatus::Success);
        QTemporaryDir output;
        QVERIFY(output.isValid());
        const QString signedPath = output.filePath(QStringLiteral("signed.pdf"));
        const auto result = backend.sign({ .page = 0,
                                           .rectangle = { .1, .1, .5, .2 },
                                           .certificateNickname = options.nickname.toStdString(),
                                           .certificateSubjectCommonName = options.commonName.toStdString(),
                                           .appearance = { } },
                                         { },
                                         signedPath);
        QVERIFY2(result.result == Mu::Model::SigningResult::Success, result.details.c_str());
        QCOMPARE(backend.open(signedPath, { }, pages), Mu::Model::OpenStatus::Success);
        backend.validateSignatures(pages, false);
        const auto findSignedField = [](const Mu::Model::PageInfo& page) {
            return std::find_if(page.signatureFields.begin(), page.signatureFields.end(), [](const auto& field) {
                return field.signedField;
            });
        };
        const auto signedField = findSignedField(pages.front());
        QVERIFY(signedField != pages.front().signatureFields.end());
        const auto field = *signedField;
        QCOMPARE(field.signatureStatus, Mu::Model::SignatureStatus::Valid);
        QCOMPARE(field.hashAlgorithm, Mu::Model::HashAlgorithm::Sha256);

        QFile signedSource(signedPath);
        QVERIFY(signedSource.open(QIODevice::ReadOnly));
        QByteArray tampered = signedSource.readAll();
        QVERIFY(tampered.startsWith("%PDF-1.") && tampered.size() > 7);
        tampered[7] = tampered[7] == '7' ? '6' : '7';
        QCOMPARE(backend.openData(tampered, { }, pages), Mu::Model::OpenStatus::Success);
        backend.validateSignatures(pages, false);
        const auto invalidField = findSignedField(pages.front());
        QVERIFY(invalidField != pages.front().signatureFields.end());
        QCOMPARE(invalidField->signatureStatus,
                 bits == 256 ? Mu::Model::SignatureStatus::DigestMismatch : Mu::Model::SignatureStatus::Invalid);

        if (!QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty()) {
            QTemporaryDir artifacts;
            QVERIFY(artifacts.isValid());
            QFile cmsFile(artifacts.filePath(QStringLiteral("signature.der")));
            QVERIFY(cmsFile.open(QIODevice::WriteOnly));
            QCOMPARE(cmsFile.write(reinterpret_cast<const char*>(field.cmsSignature.data()),
                                   static_cast<qint64>(field.cmsSignature.size())),
                     static_cast<qint64>(field.cmsSignature.size()));
            cmsFile.close();
            QFile contentFile(artifacts.filePath(QStringLiteral("content.bin")));
            QVERIFY(contentFile.open(QIODevice::WriteOnly));
            for (std::size_t i : { std::size_t(0), std::size_t(2) }) {
                QVERIFY(signedSource.seek(field.byteRange[i]));
                const auto content = signedSource.read(field.byteRange[i + 1]);
                QCOMPARE(content.size(), field.byteRange[i + 1]);
                QCOMPARE(contentFile.write(content), content.size());
            }
            contentFile.close();
            QVERIFY(runOpenSsl({ "cms",
                                 "-verify",
                                 "-binary",
                                 "-inform",
                                 "DER",
                                 "-in",
                                 cmsFile.fileName(),
                                 "-content",
                                 contentFile.fileName(),
                                 "-noverify",
                                 "-out",
                                 artifacts.filePath("verified.bin") }));
        }

        // Export/import preserves the key, including EC private-key parameters.
        QTemporaryDir bundleDir;
        QVERIFY(bundleDir.isValid());
        const QString bundlePath = bundleDir.filePath(QStringLiteral("identity.p12"));
        QVERIFY(runPk12util(
            { "-o", bundlePath, "-n", options.nickname, "-d", QStringLiteral("sql:") + m_nssDb.path(), "-W", "pass" }));
        QVERIFY2(deleteCertificate(m_nssDb.path(), certificate->identity, &error), qPrintable(error));
        SECItem keyId { siBuffer,
                        reinterpret_cast<unsigned char*>(keyIdBytes.data()),
                        static_cast<unsigned int>(keyIdBytes.size()) };
        paperbox::nss::NssPrivateKey removedKey(PK11_FindKeyByKeyID(signingSlot.get(), &keyId, nullptr));
        QVERIFY(!removedKey);
        QFile bundle(bundlePath);
        QVERIFY(bundle.open(QIODevice::ReadOnly));
        QVERIFY2(importPkcs12(m_nssDb.path(), bundle.readAll(), QStringLiteral("pass"), &error), qPrintable(error));
        QByteArray importedContent = QByteArrayLiteral("imported signing identity");
        const auto digestBytes = QCryptographicHash::hash(importedContent, QCryptographicHash::Sha256);
        std::array<std::uint8_t, 32> importedDigest { };
        std::memcpy(importedDigest.data(), digestBytes.constData(), importedDigest.size());
        const auto cms = paperbox::nss::makeSigningProvider({ })({ options.nickname.toStdString(), importedDigest });
        QCOMPARE(cms.result, Mu::Model::SigningResult::Success);
        QVERIFY(!cms.cms.empty());
        const auto imported = listCertificates(m_nssDb.path(), &error);
        const auto restored = findManagedCertificate(imported, options.nickname.toStdString());
        QVERIFY(restored != imported.cend());
        QCOMPARE(restored->certificate.publicKeyStrength, bits);
        QVERIFY2(deleteCertificate(m_nssDb.path(), restored->identity, &error), qPrintable(error));
        removedKey.reset(PK11_FindKeyByKeyID(signingSlot.get(), &keyId, nullptr));
        QVERIFY(!removedKey);
        paperbox::nss::NssCertificate removed(
            CERT_FindCertByNickname(CERT_GetDefaultCertDB(), options.nickname.toUtf8().constData()));
        QVERIFY(!removed);
    }

    void deletesCertificateByInternalSlotIdentity()
    {
        const QString firstNickname = QStringLiteral("okular-mupdf-manager-first");
        const QString secondNickname = QStringLiteral("okular-mupdf-manager-second");
        QString error;
        ::Mu::Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions options;
        options.commonName = firstNickname;
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addYears(1);
        options.nickname = firstNickname;
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error),
            qPrintable(error));
        options.nickname = secondNickname;
        options.commonName = secondNickname;
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error),
            qPrintable(error));

        const auto certificates = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto first = findManagedCertificate(certificates, firstNickname.toStdString());
        const auto second = findManagedCertificate(certificates, secondNickname.toStdString());
        QVERIFY(first != certificates.cend());
        QVERIFY(second != certificates.cend());

        PK11SlotInfo* internalSlot = PK11_GetInternalKeySlot();
        QVERIFY(internalSlot);
        QCOMPARE(first->identity.moduleId, static_cast<quint64>(PK11_GetModuleID(internalSlot)));
        QCOMPARE(first->identity.slotId, static_cast<quint64>(PK11_GetSlotID(internalSlot)));
        PK11_FreeSlot(internalSlot);

        const QByteArray firstDer(reinterpret_cast<const char*>(first->certificate.der.data()),
                                  static_cast<int>(first->certificate.der.size()));
        QCOMPARE(first->identity.sha256Fingerprint, QCryptographicHash::hash(firstDer, QCryptographicHash::Sha256));

        auto wrongSlot = first->identity;
        wrongSlot.slotId ^= 1;
        auto wrongFingerprint = first->identity;
        wrongFingerprint.sha256Fingerprint[0] ^= 1;
        const std::array invalidIdentities { wrongSlot, wrongFingerprint };
        for (const auto& identity : invalidIdentities) {
            QVERIFY(!::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), identity, &error));
            QVERIFY2(error.contains(QStringLiteral("not found"), Qt::CaseInsensitive), qPrintable(error));
        }

        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), first->identity, &error),
                 qPrintable(error));
        const auto afterFirstDelete =
            ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(findManagedCertificate(afterFirstDelete, firstNickname.toStdString()) == afterFirstDelete.cend());
        const auto remaining = findManagedCertificate(afterFirstDelete, secondNickname.toStdString());
        QVERIFY(remaining != afterFirstDelete.cend());
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), remaining->identity, &error),
            qPrintable(error));
    }

    void importsPublicTrustedSigner_data()
    {
        QTest::addColumn<bool>("pem");
        QTest::newRow("pem") << true;
        QTest::newRow("der") << false;
    }

    void importsPublicTrustedSigner()
    {
        using namespace Mu::Plugin::Crypto::CertificateDatabase;
        QFETCH(bool, pem);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto keyPath = root.filePath("key.pem");
        const auto certPath = root.filePath("cert.pem");
        QVERIFY(runOpenSsl({ "req",
                             "-x509",
                             "-newkey",
                             "ec",
                             "-pkeyopt",
                             "ec_paramgen_curve:prime256v1",
                             "-nodes",
                             "-keyout",
                             keyPath,
                             "-out",
                             certPath,
                             "-subj",
                             "/CN=Synthetic Public Signer",
                             "-days",
                             "1",
                             "-addext",
                             "basicConstraints=critical,CA:FALSE",
                             "-addext",
                             "keyUsage=critical,digitalSignature",
                             "-addext",
                             "extendedKeyUsage=emailProtection" }));
        QFile certificateFile(certPath);
        QVERIFY(certificateFile.open(QIODevice::ReadOnly));
        const QByteArray pemData = certificateFile.readAll();
        QString error;
        const auto inspected = inspectSignerCertificate(m_nssDb.path(), pemData, &error);
        QVERIFY2(inspected.has_value(), qPrintable(error));
        const QByteArray der(reinterpret_cast<const char*>(inspected->der.data()),
                             static_cast<qsizetype>(inspected->der.size()));
        QVERIFY(!inspectSignerCertificate(m_nssDb.path(), der + QByteArray("trailing"), &error));
        QVERIFY(!inspectSignerCertificate(m_nssDb.path(), der + der, &error));
        QVERIFY(!inspectSignerCertificate(m_nssDb.path(), der.left(der.size() - 1), &error));
        QVERIFY(!inspectSignerCertificate(m_nssDb.path(), pemData + pemData, &error));
        const QByteArray data = pem ? pemData : der;
        const QString nickname = pem ? QStringLiteral("public-trusted-pem") : QStringLiteral("public-trusted-der");
        const auto signingCount = listCertificates(m_nssDb.path()).size();
        const auto trustedCount = listTrustedSigners(m_nssDb.path()).size();
        QVERIFY2(importTrustedSigner(m_nssDb.path(), data, nickname, &error), qPrintable(error));
        QCOMPARE(listCertificates(m_nssDb.path()).size(), signingCount);
        auto trusted = listTrustedSigners(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(trusted.size(), trustedCount + 1);
        auto found = findManagedCertificate(trusted, nickname.toStdString());
        QVERIFY(found != trusted.cend());
        const auto identity = found->identity;
        QCOMPARE(identity.sha256Fingerprint, QCryptographicHash::hash(der, QCryptographicHash::Sha256));
        QVERIFY2(importTrustedSigner(m_nssDb.path(), data, QStringLiteral("different-label"), &error),
                 qPrintable(error));
        QCOMPARE(listTrustedSigners(m_nssDb.path()).size(), trusted.size());
        QVERIFY(runCertutil({ "-L", "-d", "sql:" + m_nssDb.path(), "-n", nickname }));
        // Reusing a nickname for a different certificate must not replace it.
        QVERIFY(runOpenSsl({ "req",
                             "-x509",
                             "-newkey",
                             "ec",
                             "-pkeyopt",
                             "ec_paramgen_curve:prime256v1",
                             "-nodes",
                             "-keyout",
                             keyPath,
                             "-out",
                             certPath,
                             "-subj",
                             "/CN=Synthetic Other Signer",
                             "-days",
                             "1",
                             "-addext",
                             "basicConstraints=critical,CA:FALSE" }));
        certificateFile.close();
        QVERIFY(certificateFile.open(QIODevice::ReadOnly));
        QVERIFY(!importTrustedSigner(m_nssDb.path(), certificateFile.readAll(), nickname, &error));
        QVERIFY(error.contains("nickname"));
        QCOMPARE(listTrustedSigners(m_nssDb.path()).size(), trusted.size());
        auto wrongIdentity = identity;
        ++wrongIdentity.slotId;
        QVERIFY(!removeSignerTrust(m_nssDb.path(), wrongIdentity, &error));
        QVERIFY2(removeSignerTrust(m_nssDb.path(), identity, &error), qPrintable(error));
        QVERIFY2(removeSignerTrust(m_nssDb.path(), identity, &error), qPrintable(error));
        QCOMPARE(listTrustedSigners(m_nssDb.path()).size(), trustedCount);
        paperbox::nss::NssCertificate retained(
            CERT_FindCertByNickname(CERT_GetDefaultCertDB(), nickname.toUtf8().constData()));
        QVERIFY(retained);
        paperbox::nss::NssSlot slot(PK11_GetInternalKeySlot());
        paperbox::nss::NssPrivateKey key(PK11_FindPrivateKeyFromCert(slot.get(), retained.get(), nullptr));
        QVERIFY(!key);
        QVERIFY2(deleteCertificate(m_nssDb.path(), identity, &error), qPrintable(error));
    }

    void rejectsInvalidTrustedSigner_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<QString>("nickname");
        QTest::newRow("empty") << QByteArray() << QStringLiteral("test");
        QTest::newRow("invalid-der") << QByteArray("invalid") << QStringLiteral("test");
        QTest::newRow("invalid-pem") << QByteArray("-----BEGIN CERTIFICATE-----\n!\n-----END CERTIFICATE-----")
                                     << QStringLiteral("test");
        QTest::newRow("multiple-pem") << QByteArray(
            "-----BEGIN CERTIFICATE-----\nAA==\n-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\nAA==\n-----END "
            "CERTIFICATE-----") << QStringLiteral("test");
        QTest::newRow("too-large") << QByteArray(
            ::Mu::Plugin::Crypto::CertificateDatabase::MaxSignerCertificateBytes + 1, 'X')
                                   << QStringLiteral("test");
        QTest::newRow("empty-nickname") << QByteArray("invalid") << QString();
        QTest::newRow("long-nickname") << QByteArray("invalid") << QString(129, 'X');
        QTest::newRow("nul-nickname") << QByteArray("invalid") << QString(QChar(0));
    }

    void rejectsInvalidTrustedSigner()
    {
        using namespace Mu::Plugin::Crypto::CertificateDatabase;
        QFETCH(QByteArray, data);
        QFETCH(QString, nickname);
        const auto before = listTrustedSigners(m_nssDb.path());
        QString error;
        QVERIFY(!importTrustedSigner(m_nssDb.path(), data, nickname, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(listTrustedSigners(m_nssDb.path()).size(), before.size());
        QTemporaryDir inactive;
        QVERIFY(inactive.isValid());
        QVERIFY(!importTrustedSigner(inactive.path(), data, QStringLiteral("test"), &error));
        QVERIFY(!removeSignerTrust(inactive.path(), { }, &error));
    }

    void rejectsTrustedCertificateAuthority()
    {
        using namespace Mu::Plugin::Crypto::CertificateDatabase;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto path = root.filePath("ca.pem");
        QVERIFY(runOpenSsl({ "req",
                             "-x509",
                             "-newkey",
                             "ec",
                             "-pkeyopt",
                             "ec_paramgen_curve:prime256v1",
                             "-nodes",
                             "-keyout",
                             root.filePath("key.pem"),
                             "-out",
                             path,
                             "-subj",
                             "/CN=Synthetic CA",
                             "-days",
                             "1",
                             "-addext",
                             "basicConstraints=critical,CA:TRUE" }));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = file.readAll();
        QString error;
        QVERIFY(!inspectSignerCertificate(m_nssDb.path(), data, &error));
        QVERIFY(error.contains("CA certificates"));
        QVERIFY(!importTrustedSigner(m_nssDb.path(), data, QStringLiteral("rejected-ca"), &error));
        QVERIFY(error.contains("CA certificates"));
    }

    void importsPkcs12Bundles()
    {
        // Legacy NSS bundle with a unicode password (pk12util round trip)
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
                runCertutil(
                    { QStringLiteral("-D"), QStringLiteral("-n"), sourceNickname, QStringLiteral("-d"), database }),
                "certutil could not remove the source certificate");

            const auto beforeFailure = snapshotCertificateDatabase();
            QVERIFY(beforeFailure);
            QVERIFY2(!::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(
                         m_nssDb.path(), data, QStringLiteral("wrong-password"), &error),
                     "an incorrect PKCS#12 password was accepted");
            QVERIFY(!error.isEmpty());
            const auto afterFailure = snapshotCertificateDatabase();
            QVERIFY(afterFailure);
            QCOMPARE(afterFailure->certificates, beforeFailure->certificates);
            QCOMPARE(afterFailure->privateKeys, beforeFailure->privateKeys);
            QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(
                         m_nssDb.path(), data, QStringLiteral("päss-密码"), &error),
                     qPrintable(error));

            const auto certificates =
                ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const auto certificate = findManagedCertificate(certificates, "okular-mupdf-pkcs12-source");
            QVERIFY(certificate != certificates.cend());

            QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(
                         m_nssDb.path(), certificate->identity, &error),
                     qPrintable(error));
        }

        // Modern OpenSSL bundle (AES-256-CBC, SHA256 MAC). Returns early
        // without openssl so the legacy coverage above still runs there.
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
            return;
        {
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
            const auto certificates =
                ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            const auto certificate = findManagedCertificate(certificates, "modern-pkcs12-test");
            QVERIFY(certificate != certificates.cend());
            QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(
                         m_nssDb.path(), certificate->identity, &error),
                     qPrintable(error));
        }
    }

    void rejectsMalformedPkcs12WithoutChangingDatabase_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("empty") << 0;
        QTest::newRow("invalid-der") << 1;
        QTest::newRow("truncated-bundle") << 2;
        QTest::newRow("corrupted-integrity") << 3;
    }

    void rejectsMalformedPkcs12WithoutChangingDatabase()
    {
        QFETCH(int, kind);
        QTemporaryDir source;
        QVERIFY(source.isValid());
        QByteArray data;
        if (kind == 1) {
            data = "not a PKCS#12 bundle";
        } else if (kind >= 2) {
            const QString path = source.filePath(QStringLiteral("source.p12"));
            QVERIFY(runPk12util(
                { "-o", path, "-n", "okular-mupdf-test", "-d", "sql:" + m_nssDb.path(), "-K", "", "-W", "password" }));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            data = file.readAll();
            QVERIFY(data.size() > 32);
            if (kind == 2)
                data.chop(data.size() / 2);
            else
                data[data.size() - 8] = char(data.at(data.size() - 8) ^ 0x5a);
        }
        const auto before = snapshotCertificateDatabase();
        QVERIFY(before);
        const auto cleanupImports = qScopeGuard([&] { removeNewSigningCertificates(m_nssDb.path(), *before); });
        QString error;
        QVERIFY(!::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(m_nssDb.path(), data, "password", &error));
        QVERIFY(!error.isEmpty());
        const auto after = snapshotCertificateDatabase();
        QVERIFY(after);
        QCOMPARE(after->certificates, before->certificates);
        QCOMPARE(after->privateKeys, before->privateKeys);
    }

    void resolvesPkcs12NicknameCollisionWithoutReplacingIdentity()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
            QSKIP("openssl is required to generate a distinct key with a colliding nickname");
        const auto bytes =
            createPkcs12Bundle(QStringLiteral("okular-mupdf-test"), QStringLiteral("Imported Collision Signer"));
        QVERIFY(bytes && !bytes->isEmpty());
        const auto before = snapshotCertificateDatabase();
        QVERIFY(before);
        const auto cleanupImports = qScopeGuard([&] { removeNewSigningCertificates(m_nssDb.path(), *before); });
        QString error;
        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(m_nssDb.path(), *bytes, "password", &error),
                 qPrintable(error));
        const auto after = snapshotCertificateDatabase();
        QVERIFY(after);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        for (auto it = before->certificates.cbegin(); it != before->certificates.cend(); ++it)
            QCOMPARE(after->certificates.value(it.key()), it.value());
        for (const auto& identity : before->privateKeys)
            QVERIFY(after->privateKeys.contains(identity));
        QCOMPARE(after->certificates.size(), before->certificates.size() + 1);
        QCOMPARE(after->privateKeys.size(), before->privateKeys.size() + 1);
        const auto records = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto incoming = std::find_if(records.cbegin(), records.cend(), [&](const auto& record) {
            return !before->certificates.contains(record.identity.sha256Fingerprint);
        });
        QVERIFY(incoming != records.cend());
        QVERIFY(incoming->certificate.nickname != "okular-mupdf-test");
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), incoming->identity, &error),
            qPrintable(error));
        const auto restored = snapshotCertificateDatabase();
        QVERIFY(restored);
        QCOMPARE(restored->certificates, before->certificates);
        QCOMPARE(restored->privateKeys, before->privateKeys);
    }

    void rollsBackPkcs12ObjectsAfterImportFailure()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
            QSKIP("openssl is required for the PKCS#12 rollback fixture");
        const auto bytes = createPkcs12Bundle(QStringLiteral("rollback-import"), QStringLiteral("Rollback Import"));
        QVERIFY(bytes && !bytes->isEmpty());
        const auto before = snapshotCertificateDatabase();
        QVERIFY(before);
        const auto cleanupImports = qScopeGuard([&] { removeNewSigningCertificates(m_nssDb.path(), *before); });
        // Fail only after the real NSS call stored the new certificate and key.
        // The production import adapter must remove both while preserving old objects.
        failPkcs12ImportAfterWrite = true;
        const auto resetFailure = qScopeGuard([] { failPkcs12ImportAfterWrite = false; });
        QString error;
        QVERIFY(!::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(m_nssDb.path(), *bytes, "password", &error));
        QVERIFY(!failPkcs12ImportAfterWrite);
        QVERIFY2(error.contains(QStringLiteral("could not be imported")), qPrintable(error));
        const auto after = snapshotCertificateDatabase();
        QVERIFY(after);
        QCOMPARE(after->certificates, before->certificates);
        QCOMPARE(after->privateKeys, before->privateKeys);
        // The injected failure is one-shot: retrying must succeed without leftovers.
        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::importPkcs12(m_nssDb.path(), *bytes, "password", &error),
                 qPrintable(error));
        const auto records = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto incoming = findManagedCertificate(records, "rollback-import");
        QVERIFY(incoming != records.cend());
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), incoming->identity, &error),
            qPrintable(error));
        const auto restored = snapshotCertificateDatabase();
        QVERIFY(restored);
        QCOMPARE(restored->certificates, before->certificates);
        QCOMPARE(restored->privateKeys, before->privateKeys);
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

    void rejectsPublicOnlyCertificateImport_data()
    {
        QTest::addColumn<bool>("alreadyPresent");
        QTest::addColumn<QString>("importNickname");
        QTest::newRow("new certificate") << false << QStringLiteral("orphan-public-cert");
        QTest::newRow("existing certificate") << true << QStringLiteral("orphan-public-cert");
        QTest::newRow("existing certificate with different nickname") << true << QStringLiteral("renamed-public-cert");
    }

    void rejectsPublicOnlyCertificateImport()
    {
        QFETCH(bool, alreadyPresent);
        QFETCH(QString, importNickname);
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
            QSKIP("openssl is required for the public-only certificate import test");
        QTemporaryDir source;
        QVERIFY(source.isValid());
        const QString keyPath = source.filePath(QStringLiteral("key.pem"));
        const QString certPath = source.filePath(QStringLiteral("certificate.pem"));
        QVERIFY2(runOpenSsl({ QStringLiteral("req"),
                              QStringLiteral("-x509"),
                              QStringLiteral("-newkey"),
                              QStringLiteral("rsa:2048"),
                              QStringLiteral("-nodes"),
                              QStringLiteral("-keyout"),
                              keyPath,
                              QStringLiteral("-out"),
                              certPath,
                              QStringLiteral("-subj"),
                              QStringLiteral("/CN=Public Only Import Test"),
                              QStringLiteral("-days"),
                              QStringLiteral("1") }),
                 "openssl could not create the public-only certificate");

        QFile certReader(certPath);
        QVERIFY(certReader.open(QIODevice::ReadOnly));
        const QByteArray certificateData = certReader.readAll();
        certReader.close();
        QVERIFY(!certificateData.isEmpty());

        CERTCertDBHandle* certdb = CERT_GetDefaultCertDB();
        QVERIFY(certdb);
        QByteArray originalDer;
        CERTCertTrust originalTrust { };
        if (alreadyPresent) {
            QVERIFY(runCertutil({ QStringLiteral("-A"),
                                  QStringLiteral("-d"),
                                  QStringLiteral("sql:") + m_nssDb.path(),
                                  QStringLiteral("-n"),
                                  QStringLiteral("orphan-public-cert"),
                                  QStringLiteral("-t"),
                                  QStringLiteral("CT,C,C"),
                                  QStringLiteral("-i"),
                                  certPath }));
            paperbox::nss::NssCertificate existing(CERT_FindCertByNickname(certdb, "orphan-public-cert"));
            QVERIFY(existing);
            originalDer = QByteArray(reinterpret_cast<const char*>(existing->derCert.data), existing->derCert.len);
            QVERIFY(CERT_GetCertTrust(existing.get(), &originalTrust) == SECSuccess);
        }

        QString error;
        // The key lives outside NSS; rejection must not mutate the database.
        const bool imported = ::Mu::Plugin::Crypto::CertificateDatabase::importCertificate(
            m_nssDb.path(), certificateData, importNickname, &error);
        QVERIFY(!imported);
        QVERIFY2(error.contains(QStringLiteral("has no associated private key")), qPrintable(error));

        // Public-only certificates must never appear as signing identities.
        const auto signingCerts = ::Mu::Plugin::Crypto::signingCertificates();
        const auto it = std::find_if(signingCerts.cbegin(), signingCerts.cend(), [](const auto& cert) {
            return cert.nickname == "orphan-public-cert";
        });
        QVERIFY(it == signingCerts.cend());
        paperbox::nss::NssCertificate certificate(CERT_FindCertByNickname(certdb, "orphan-public-cert"));
        if (alreadyPresent) {
            QVERIFY(certificate);
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(certificate->derCert.data), certificate->derCert.len),
                     originalDer);
            CERTCertTrust trust { };
            QVERIFY(CERT_GetCertTrust(certificate.get(), &trust) == SECSuccess);
            QCOMPARE(trust.sslFlags, originalTrust.sslFlags);
            QCOMPARE(trust.emailFlags, originalTrust.emailFlags);
            QCOMPARE(trust.objectSigningFlags, originalTrust.objectSigningFlags);
            if (importNickname != QStringLiteral("orphan-public-cert")) {
                paperbox::nss::NssCertificate renamed(
                    CERT_FindCertByNickname(certdb, importNickname.toUtf8().constData()));
                QVERIFY(!renamed);
            }
            QVERIFY(SEC_DeletePermCertificate(certificate.get()) == SECSuccess);
        } else {
            QVERIFY(!certificate);
        }
    }

    void importsCertificateWithExistingPrivateKey()
    {
        const QString nickname = QStringLiteral("pem-existing-key");
        ::Mu::Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions options;
        options.nickname = nickname;
        options.commonName = QStringLiteral("PEM Existing Key Test");
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addYears(1);
        QString error;
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(m_nssDb.path(), options, &error),
            qPrintable(error));

        QTemporaryDir source;
        QVERIFY(source.isValid());
        const QString certPath = source.filePath(QStringLiteral("certificate.pem"));
        const QString database = QStringLiteral("sql:") + m_nssDb.path();
        QVERIFY(runCertutil({ QStringLiteral("-L"),
                              QStringLiteral("-d"),
                              database,
                              QStringLiteral("-n"),
                              nickname,
                              QStringLiteral("-a"),
                              QStringLiteral("-o"),
                              certPath }));
        QFile file(certPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray certificateData = file.readAll();
        QVERIFY(!certificateData.isEmpty());
        // certutil -D removes only the certificate, retaining its token key.
        QVERIFY(runCertutil({ QStringLiteral("-D"), QStringLiteral("-d"), database, QStringLiteral("-n"), nickname }));
        QVERIFY2(::Mu::Plugin::Crypto::CertificateDatabase::importCertificate(
                     m_nssDb.path(), certificateData, nickname, &error),
                 qPrintable(error));
        const auto cms = paperbox::nss::makeSigningProvider({ })({ nickname.toStdString(), { } });
        QCOMPARE(cms.result, ::Mu::Model::SigningResult::Success);

        const auto certificates = ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(m_nssDb.path(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto certificate = findManagedCertificate(certificates, nickname.toStdString());
        QVERIFY(certificate != certificates.cend());
        QVERIFY2(
            ::Mu::Plugin::Crypto::CertificateDatabase::deleteCertificate(m_nssDb.path(), certificate->identity, &error),
            qPrintable(error));
    }

    void rejectsUnsupportedSigningKey_data()
    {
        QTest::addColumn<int>("key");
        QTest::newRow("negative") << -1;
        QTest::newRow("unknown") << 4;
        QTest::newRow("large") << 999;
    }

    void rejectsUnsupportedSigningKey()
    {
        using namespace Mu::Plugin::Crypto::CertificateDatabase;
        QFETCH(int, key);
        const auto countKeys = [] {
            paperbox::nss::NssSlot slot(PK11_GetInternalKeySlot());
            SECKEYPrivateKeyList* keys = PK11_ListPrivateKeysInSlot(slot.get());
            int count = 0;
            if (keys) {
                for (auto* node = PR_LIST_HEAD(&keys->list); node != &keys->list; node = PR_NEXT_LINK(node))
                    ++count;
                SECKEY_DestroyPrivateKeyList(keys);
            }
            return count;
        };
        const auto certificatesBefore = listCertificates(m_nssDb.path());
        const int keysBefore = countKeys();
        SelfSignedCertificateOptions options;
        options.nickname = QStringLiteral("unsupported-key");
        options.commonName = options.nickname;
        options.validFrom = QDateTime::currentDateTime().addSecs(-60);
        options.validUntil = options.validFrom.addYears(1);
        options.signingKey = static_cast<SigningKey>(key);
        QString error;
        QVERIFY(!createSelfSignedCertificate(m_nssDb.path(), options, &error));
        QVERIFY(error.contains(QStringLiteral("unsupported")));
        QCOMPARE(listCertificates(m_nssDb.path()).size(), certificatesBefore.size());
        QCOMPARE(countKeys(), keysBefore);
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
        for (const QString& invalidEmail : { QStringLiteral("local-only"),
                                             QStringLiteral("@example.org"),
                                             QStringLiteral("user@"),
                                             QStringLiteral("a@@example.org"),
                                             QStringLiteral("invalid email@example.org") }) {
            options.email = invalidEmail;
            error.clear();
            QVERIFY2(!::Mu::Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(
                         m_nssDb.path(), options, &error),
                     qPrintable(invalidEmail));
            QVERIFY2(error.contains(QStringLiteral("email address is invalid")), qPrintable(error));
        }

        options.email.clear();
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
};

QTEST_GUILESS_MAIN(TestCertificateDatabase)
#include "test_certificate_database.moc"
