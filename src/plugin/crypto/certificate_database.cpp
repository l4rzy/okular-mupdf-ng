// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/crypto/certificate_database.hpp"

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <certdb.h>
#include <cryptohi.h>
#include <keyhi.h>
#include <p12.h>
#include <pk11pub.h>
#include <prerror.h>
#include <secasn1.h>
#include <secerr.h>
#include <secoid.h>
#include <secport.h>
#pragma pop_macro("slots")

#include <QDateTime>
#include <QRandomGenerator>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

#include "plugin/crypto/certificate_info_internal.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/nss_error_internal.hpp"
#include "plugin/crypto/nss_handles.hpp"
#include "plugin/crypto/nss_internal.hpp"
#include "shared/logging.hpp"

namespace Mu::Plugin::Crypto::CertificateDatabase {

namespace {

PRBool
pkcs12UnicodeAsciiConversion(PRBool, unsigned char*, unsigned int, unsigned char*, unsigned int, unsigned int*, PRBool);

// RAII guard for NSS process-global UCS-2 / ASCII conversion function.
// Note: NSS does not provide an API to query the previous conversion function pointer.
// The guard serializes exclusive process-wide access via Internal::nssMutex().
class Pkcs12ConversionGuard {
public:
    explicit Pkcs12ConversionGuard(std::mutex& mutex)
        : m_lock(mutex)
    {
        PORT_SetUCS2_ASCIIConversionFunction(pkcs12UnicodeAsciiConversion);
    }

    ~Pkcs12ConversionGuard() { PORT_SetUCS2_ASCIIConversionFunction(PORT_UCS2_ASCIIConversion); }

    Pkcs12ConversionGuard(const Pkcs12ConversionGuard&) = delete;
    Pkcs12ConversionGuard& operator=(const Pkcs12ConversionGuard&) = delete;

private:
    std::lock_guard<std::mutex> m_lock;
};

using SensitiveBytes = Plugin::Crypto::SensitiveBytes;

void setError(QString* error, const QString& message)
{
    if (error)
        *error = message;
}

void clearError(QString* error)
{
    if (error)
        error->clear();
}

enum class DatabaseAccess {
    ReadOnly,
    ReadWrite,
};

bool ensureCertificateDatabase(const QString& databasePath, DatabaseAccess access, QString* error)
{
    const auto requestedMode = Plugin::Crypto::initializeNss(databasePath);
    if (requestedMode == Plugin::Crypto::NssRuntimeMode::ReadWrite)
        return true;
    if (requestedMode == Plugin::Crypto::NssRuntimeMode::ReadOnly) {
        if (access == DatabaseAccess::ReadOnly)
            return true;
        setError(error, QStringLiteral("The active NSS certificate database is read-only"));
        return false;
    }

    const auto activeMode = Plugin::Crypto::activeNssMode();
    if (activeMode == Plugin::Crypto::NssRuntimeMode::ReadOnly
        || activeMode == Plugin::Crypto::NssRuntimeMode::ReadWrite)
        setError(error, QStringLiteral("The selected NSS database is not active; restart Okular after changing it"));
    else
        setError(error, QStringLiteral("A persistent NSS certificate database is unavailable"));
    return false;
}

QString nssError()
{
    return Plugin::Crypto::Internal::nssErrorMessage(PR_GetError());
}

QString nssError(PRErrorCode error)
{
    return Plugin::Crypto::Internal::nssErrorMessage(error);
}

bool convertToNssTime(const QDateTime& dateTime, PRTime* result)
{
    // NSS stores time in microseconds, so validate the millisecond conversion
    // before multiplying to avoid signed overflow.
    if (!result || !dateTime.isValid())
        return false;
    const qint64 milliseconds = dateTime.toMSecsSinceEpoch();
    if (milliseconds > std::numeric_limits<qint64>::max() / 1000
        || milliseconds < std::numeric_limits<qint64>::min() / 1000)
        return false;
    *result = milliseconds * 1000;
    return true;
}

std::optional<QString> escapeDnValue(const QString& value)
{
    // Use NSS's RFC 1485 escaping so commas, quotes, and control characters
    // cannot change the structure of the generated distinguished name.
    const QByteArray source = value.toUtf8();
    if (source.size() > (std::numeric_limits<int>::max() - 3) / 4)
        return std::nullopt;
    QByteArray escaped(source.size() * 4 + 3, '\0');
    if (CERT_RFC1485_EscapeAndQuote(escaped.data(),
                                    static_cast<int>(escaped.size()),
                                    const_cast<char*>(source.constData()),
                                    static_cast<int>(source.size()))
        != SECSuccess)
        return std::nullopt;
    return QString::fromUtf8(escaped.constData());
}

QByteArray encodePkcs12Password(const QString& password)
{
    // SEC_PKCS12DecoderStart expects the password in the BMP/UCS-2 form used
    // by the PKCS#12 PBE algorithms: big-endian UTF-16 code units followed by
    // a two-byte terminator.  The decoder does not perform this conversion for
    // those algorithms itself.
    QByteArray encoded;
    encoded.reserve((password.size() + 1) * 2);
    for (const QChar character : password) {
        const ushort value = character.unicode();
        encoded.append(static_cast<char>(value >> 8));
        encoded.append(static_cast<char>(value & 0xff));
    }
    encoded.append('\0');
    encoded.append('\0');
    return encoded;
}

SECItem* pkcs12NicknameCollision(SECItem* oldNickname, PRBool* cancel, void* argument)
{
    // PKCS#12 import invokes this callback for duplicate nicknames; cancel the
    // import instead of silently replacing an existing signing identity.
    auto setCancel = [&](PRBool value) {
        if (cancel)
            *cancel = value;
    };
    setCancel(PR_FALSE);

    auto* certificate = static_cast<CERTCertificate*>(argument);
    if (!certificate) {
        setCancel(PR_TRUE);
        return nullptr;
    }

    char* nickname = CERT_MakeCANickname(certificate);
    if (!nickname) {
        setCancel(PR_TRUE);
        return nullptr;
    }
    const size_t nicknameLength = std::strlen(nickname);
    if (nicknameLength > std::numeric_limits<unsigned int>::max()) {
        PORT_Free(nickname);
        setCancel(PR_TRUE);
        return nullptr;
    }
    if (oldNickname && oldNickname->data && oldNickname->len == nicknameLength
        && std::memcmp(oldNickname->data, nickname, nicknameLength) == 0) {
        PORT_Free(nickname);
        PORT_SetError(SEC_ERROR_IO);
        setCancel(PR_TRUE);
        return nullptr;
    }

    SECItem* replacement = PORT_ZNew(SECItem);
    if (!replacement) {
        PORT_Free(nickname);
        setCancel(PR_TRUE);
        return nullptr;
    }
    replacement->type = siAsciiString;
    replacement->data = reinterpret_cast<unsigned char*>(nickname);
    replacement->len = static_cast<unsigned int>(nicknameLength);
    return replacement;
}

PRBool pkcs12UnicodeAsciiConversion(PRBool,
                                    unsigned char* input,
                                    unsigned int inputLength,
                                    unsigned char* output,
                                    unsigned int outputCapacity,
                                    unsigned int* outputLength,
                                    PRBool)
{
    // PKCS#12 receives pre-encoded password bytes, so preserve them rather
    // than applying NSS's default character conversion a second time.
    if (!input || !output || !outputLength || outputCapacity < inputLength)
        return PR_FALSE;
    std::memcpy(output, input, inputLength);
    *outputLength = inputLength;
    return PR_TRUE;
}

using SlotHandle = Plugin::Crypto::NssSlot;
using CertificateHandle = Plugin::Crypto::NssCertificate;
using PrivateKeyHandle = Plugin::Crypto::NssPrivateKey;
using PublicKeyHandle = Plugin::Crypto::NssPublicKey;
using DecoderHandle = Plugin::Crypto::NssPkcs12Decoder;

using ArenaHandle = Plugin::Crypto::NssArena;

bool checkSelfSignedOptions(const SelfSignedCertificateOptions& options,
                            QString* nickname,
                            QString* commonName,
                            QString* country,
                            PRTime* validFrom,
                            PRTime* validUntil,
                            QString* error)
{
    // Validate all user-controlled subject fields and dates before allocating
    // NSS objects or creating persistent key material.
    *nickname = options.nickname.trimmed();
    *commonName = options.commonName.trimmed();
    *country = options.country.trimmed();
    const QList<QString> subjectValues { *nickname,
                                         *commonName,
                                         options.organization.trimmed(),
                                         options.organizationalUnit.trimmed(),
                                         options.locality.trimmed(),
                                         options.state.trimmed(),
                                         *country };
    if (nickname->isEmpty() || commonName->isEmpty()) {
        setError(error, QStringLiteral("A nickname and Common Name are required"));
        return false;
    }
    if (std::any_of(
            subjectValues.cbegin(), subjectValues.cend(), [](const QString& value) { return value.size() > 256; })) {
        setError(error, QStringLiteral("Certificate fields must be 256 characters or shorter"));
        return false;
    }
    if (nickname->size() > 128) {
        setError(error, QStringLiteral("The certificate nickname must be 128 characters or shorter"));
        return false;
    }
    if (!convertToNssTime(options.validFrom, validFrom) || !convertToNssTime(options.validUntil, validUntil)
        || options.validUntil <= options.validFrom) {
        setError(error, QStringLiteral("The certificate expiration must be after its start date"));
        return false;
    }
    if (!country->isEmpty()
        && (country->size() != 2 || !std::all_of(country->cbegin(), country->cend(), [](QChar character) {
                return character.unicode() < 128 && character.isLetter();
            }))) {
        setError(error, QStringLiteral("The country code must contain exactly two characters"));
        return false;
    }
    return true;
}

QByteArray buildDistinguishedNameBytes(const SelfSignedCertificateOptions& options,
                                       const QString& commonName,
                                       const QString& country,
                                       bool* validSubject)
{
    // Build the subject from escaped components so user text cannot inject
    // additional distinguished-name attributes.
    auto escapedOrInvalid = [&](const QString& rawValue) -> std::optional<QString> {
        const auto escaped = escapeDnValue(rawValue);
        if (!escaped)
            *validSubject = false;
        return escaped;
    };
    auto appendDn = [&](QStringList* dn, const QString& prefix, const QString& rawValue) {
        const QString value = rawValue.trimmed();
        if (value.isEmpty())
            return;
        if (auto escaped = escapedOrInvalid(value))
            dn->append(prefix + *escaped);
    };

    auto cn = escapedOrInvalid(commonName);
    if (!cn)
        return { };
    QStringList distinguishedName { QStringLiteral("CN=%1").arg(*cn) };
    appendDn(&distinguishedName, QStringLiteral("O="), options.organization);
    appendDn(&distinguishedName, QStringLiteral("OU="), options.organizationalUnit);
    appendDn(&distinguishedName, QStringLiteral("L="), options.locality);
    appendDn(&distinguishedName, QStringLiteral("ST="), options.state);
    if (!country.isEmpty())
        distinguishedName.append(QStringLiteral("C=%1").arg(country.toUpper()));
    return distinguishedName.join(QStringLiteral(", ")).toUtf8();
}

bool generateRsaKeypair(SlotHandle& slot, PrivateKeyHandle* privateKey, PublicKeyHandle* publicKey, QString* error)
{
    // Generate both halves as persistent token objects because NSS signing
    // later resolves the private key through the stored certificate.
    PK11RSAGenParams rsaParameters { 2048, 0x10001 };
    SECKEYPublicKey* pubKey = nullptr;
    SECKEYPrivateKey* privKey =
        PK11_GenerateKeyPair(slot.get(), CKM_RSA_PKCS_KEY_PAIR_GEN, &rsaParameters, &pubKey, PR_TRUE, PR_TRUE, nullptr);
    privateKey->reset(privKey);
    publicKey->reset(pubKey);
    if (!privKey || !pubKey) {
        if (privKey)
            PK11_DeleteTokenPrivateKey(privKey, PR_TRUE);
        if (pubKey)
            PK11_DeleteTokenPublicKey(pubKey);
        setError(error, QStringLiteral("Could not generate the RSA signing key"));
        return false;
    }
    return true;
}

bool addSelfSignedExtensions(CERTCertificate* certificate, QString* error)
{
    // Mark the certificate for signing while keeping the extension encoding
    // under NSS ownership until the certificate is finalized.
    const unsigned char basicConstraintsBytes[] { 0x30, 0x00 };
    const unsigned char keyUsageBytes[] { 0x03, 0x02, 0x05, 0xc0 };
    void* extensions = CERT_StartCertExtensions(certificate);
    SECItem basicConstraints { siBuffer,
                               const_cast<unsigned char*>(basicConstraintsBytes),
                               sizeof(basicConstraintsBytes) };
    SECItem keyUsage { siBuffer, const_cast<unsigned char*>(keyUsageBytes), sizeof(keyUsageBytes) };
    const bool added = extensions
        && CERT_AddExtension(extensions, SEC_OID_X509_BASIC_CONSTRAINTS, &basicConstraints, PR_TRUE, PR_TRUE)
            == SECSuccess
        && CERT_AddExtension(extensions, SEC_OID_X509_KEY_USAGE, &keyUsage, PR_TRUE, PR_TRUE) == SECSuccess
        && CERT_FinishExtensions(extensions) == SECSuccess;
    if (!added) {
        setError(error, QStringLiteral("Could not add signing extensions to the certificate"));
        return false;
    }
    return true;
}

bool encodeSignAndImport(SlotHandle& slot,
                         CertificateHandle& unsignedHandle,
                         PrivateKeyHandle& privateKey,
                         PublicKeyHandle& publicKey,
                         const QString& nickname,
                         QString* error)
{
    // Encode, sign, and import as one operation; callers delete both token
    // keys if the certificate cannot be stored.
    ArenaHandle arena(PORT_NewArena(DER_DEFAULT_CHUNKSIZE));
    SECItem unsignedData { siBuffer, nullptr, 0 };
    SECAlgorithmID signatureAlgorithm { };
    SECItem derCertificate { siBuffer, nullptr, 0 };
    bool encoded = arena.get()
        && SECOID_SetAlgorithmID(arena.get(), &signatureAlgorithm, SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION, nullptr)
            == SECSuccess;
    if (encoded) {
        unsignedHandle->signature = signatureAlgorithm;
        encoded = SEC_ASN1EncodeItem(arena.get(), &unsignedData, unsignedHandle.get(), CERT_CertificateTemplate);
    }
    if (encoded) {
        encoded = SEC_DerSignDataWithAlgorithmID(arena.get(),
                                                 &derCertificate,
                                                 unsignedData.data,
                                                 static_cast<int>(unsignedData.len),
                                                 privateKey.get(),
                                                 &signatureAlgorithm)
            == SECSuccess;
        unsignedHandle->signature = { };
    }
    CertificateHandle certificate(
        encoded ? CERT_NewTempCertificate(CERT_GetDefaultCertDB(), &derCertificate, nullptr, PR_FALSE, PR_TRUE)
                : nullptr);
    const QByteArray nicknameBytes = nickname.toUtf8();
    const bool imported = certificate
        && PK11_ImportCert(slot.get(), certificate.get(), privateKey->pkcs11ID, nicknameBytes.constData(), PR_FALSE)
            == SECSuccess;
    const QString importError = imported ? QString { } : nssError();
    if (!imported)
        ::Mu::Plugin::Crypto::deleteTokenKeypair(privateKey.get(), publicKey.get());
    if (!imported) {
        const QString action = certificate ? QStringLiteral("store the self-signed certificate in the NSS database")
                                           : QStringLiteral("decode the generated self-signed certificate");
        setError(error, QStringLiteral("Could not %1: %2").arg(action, importError));
        return false;
    }
    return true;
}

} // namespace

QList<Model::Certificate> listCertificates(const QString& databasePath, QString* error)
{
    clearError(error);
    if (!ensureCertificateDatabase(databasePath, DatabaseAccess::ReadOnly, error))
        return { };
    std::lock_guard<std::mutex> lock(Plugin::Crypto::Internal::nssMutex());
    return Plugin::Crypto::Internal::listSigningCertificates();
}

bool importCertificate(const QString& databasePath, const QByteArray& data, const QString& nickname, QString* error)
{
    clearError(error);
    if (data.isEmpty() || nickname.trimmed().isEmpty()) {
        setError(error, QStringLiteral("A certificate and nickname are required"));
        return false;
    }
    if (static_cast<quint64>(data.size()) > static_cast<quint64>(std::numeric_limits<int>::max())) {
        setError(error, QStringLiteral("The certificate data is too large"));
        return false;
    }
    if (!ensureCertificateDatabase(databasePath, DatabaseAccess::ReadWrite, error))
        return false;
    std::lock_guard<std::mutex> lock(Plugin::Crypto::Internal::nssMutex());
    QByteArray package = data;
    CertificateHandle certificate(CERT_DecodeCertFromPackage(package.data(), static_cast<int>(package.size())));
    if (!certificate) {
        setError(error, QStringLiteral("The certificate data is invalid"));
        return false;
    }
    SlotHandle slot(PK11_GetInternalKeySlot());
    if (!slot) {
        setError(error, nssError());
        return false;
    }
    const QByteArray name = nickname.trimmed().toUtf8();
    const SECStatus status =
        PK11_ImportCert(slot.get(), certificate.get(), CK_INVALID_HANDLE, name.constData(), PR_FALSE);
    if (status != SECSuccess) {
        setError(error, nssError());
        return false;
    }
    CERTCertDBHandle* database = CERT_GetDefaultCertDB();
    CertificateHandle imported = ::Mu::Plugin::Crypto::findCertificateByNickname(database, nickname.trimmed());
    PrivateKeyHandle key(imported ? PK11_FindKeyByAnyCert(imported.get(), nullptr) : nullptr);
    if (!key) {
        if (imported) {
            const SECStatus deleteStatus = ::Mu::Plugin::Crypto::deleteCertificateAndKeys(imported.get());
            if (deleteStatus != SECSuccess) {
                setError(error,
                         QStringLiteral("The certificate has no private key, and rollback failed: %1").arg(nssError()));
                return false;
            }
        }
        setError(error, QStringLiteral("The imported certificate has no associated private key"));
        return false;
    }
    return true;
}

bool importPkcs12(const QString& databasePath, const QByteArray& data, const QString& password, QString* error)
{
    clearError(error);
    if (data.isEmpty()) {
        setError(error, QStringLiteral("The PKCS#12 bundle is empty"));
        return false;
    }
    if (static_cast<quint64>(data.size()) > std::numeric_limits<unsigned long>::max()) {
        setError(error, QStringLiteral("The PKCS#12 bundle is too large"));
        return false;
    }
    if (!ensureCertificateDatabase(databasePath, DatabaseAccess::ReadWrite, error))
        return false;

    SensitiveBytes passwordBytes(encodePkcs12Password(password));
    SECItem passwordItem { siBuffer,
                           reinterpret_cast<unsigned char*>(passwordBytes.bytes.data()),
                           static_cast<unsigned int>(passwordBytes.bytes.size()) };
    // NSS receives an already-Unicode password below. Keep its legacy
    // UCS-2/ASCII conversion from transforming those bytes a second time.
    {
        Pkcs12ConversionGuard conversionGuard(Plugin::Crypto::Internal::nssMutex());
        SlotHandle slot(PK11_GetInternalKeySlot());
        if (!slot) {
            setError(error, nssError());
            return false;
        }
        SEC_PKCS12DecoderContext* decoder =
            SEC_PKCS12DecoderStart(&passwordItem, slot.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!decoder) {
            setError(error, QStringLiteral("Could not open the PKCS#12 bundle; check its password"));
            return false;
        }
        DecoderHandle decoderHandle(decoder);

        const bool updated = SEC_PKCS12DecoderUpdate(
                                 decoderHandle.get(),
                                 const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(data.constData())),
                                 static_cast<unsigned long>(data.size()))
            == SECSuccess;
        const PRErrorCode updateError = updated ? 0 : PR_GetError();
        if (!updated) {
            setError(error, QStringLiteral("Could not read the PKCS#12 bundle: %1").arg(nssError(updateError)));
            return false;
        }
        const bool verified = SEC_PKCS12DecoderVerify(decoderHandle.get()) == SECSuccess;
        const PRErrorCode verifyError = verified ? 0 : PR_GetError();
        if (!verified) {
            setError(error,
                     QStringLiteral("The PKCS#12 password is incorrect or its integrity check failed: %1")
                         .arg(nssError(verifyError)));
            return false;
        }
        const bool validated =
            SEC_PKCS12DecoderValidateBags(decoderHandle.get(), pkcs12NicknameCollision) == SECSuccess;
        const PRErrorCode validateError = validated ? 0 : PR_GetError();
        if (!validated) {
            setError(error,
                     QStringLiteral("The PKCS#12 bundle is invalid or unsupported: %1").arg(nssError(validateError)));
            return false;
        }
        const QSet<QString> beforeImport = [&] {
            QSet<QString> names;
            for (const auto& certificate : Plugin::Crypto::Internal::listSigningCertificates())
                names.insert(QString::fromStdString(certificate.nickname));
            return names;
        }();
        const bool imported = SEC_PKCS12DecoderImportBags(decoderHandle.get()) == SECSuccess;
        if (!imported) {
            const PRErrorCode importError = PR_GetError();
            for (const auto& certificate : Plugin::Crypto::Internal::listSigningCertificates()) {
                const QString nickname = QString::fromStdString(certificate.nickname);
                if (beforeImport.contains(nickname))
                    continue;
                CERTCertDBHandle* db = CERT_GetDefaultCertDB();
                CertificateHandle cert = ::Mu::Plugin::Crypto::findCertificateByNickname(db, nickname);
                if (cert && ::Mu::Plugin::Crypto::deleteCertificateAndKeys(cert.get()) != SECSuccess) {
                    MU_LOG(warning,
                           "Mu::Generator::CertificateManager",
                           std::string("Could not roll back imported certificate: ") + nssError().toStdString());
                }
            }
            setError(error, QStringLiteral("The PKCS#12 bundle could not be imported: %1").arg(nssError(importError)));
            return false;
        }
    }

    return true;
}

bool createSelfSignedCertificate(const QString& databasePath,
                                 const SelfSignedCertificateOptions& options,
                                 QString* error)
{
    clearError(error);
    QString nickname;
    QString commonName;
    QString country;
    PRTime validFrom = 0;
    PRTime validUntil = 0;
    if (!checkSelfSignedOptions(options, &nickname, &commonName, &country, &validFrom, &validUntil, error))
        return false;
    if (!ensureCertificateDatabase(databasePath, DatabaseAccess::ReadWrite, error))
        return false;
    std::lock_guard<std::mutex> lock(Plugin::Crypto::Internal::nssMutex());

    bool validSubject = true;
    const QByteArray distinguishedNameBytes = buildDistinguishedNameBytes(options, commonName, country, &validSubject);
    if (!validSubject) {
        setError(error, QStringLiteral("The certificate subject contains invalid text"));
        return false;
    }
    CERTName* subject = CERT_AsciiToName(distinguishedNameBytes.constData());
    if (!subject) {
        setError(error, QStringLiteral("The certificate subject is invalid"));
        return false;
    }
    ArenaHandle subjectArena(subject->arena);

    SlotHandle slot(PK11_GetInternalKeySlot());
    if (!slot) {
        setError(error, nssError());
        return false;
    }
    PrivateKeyHandle privateKeyHandle;
    PublicKeyHandle publicKeyHandle;
    if (!generateRsaKeypair(slot, &privateKeyHandle, &publicKeyHandle, error))
        return false;

    Plugin::Crypto::NssSubjectPublicKeyInfo publicKeyInfo(SECKEY_CreateSubjectPublicKeyInfo(publicKeyHandle.get()));
    Plugin::Crypto::NssCertificateRequest request(
        publicKeyInfo ? CERT_CreateCertificateRequest(subject, publicKeyInfo.get(), nullptr) : nullptr);
    Plugin::Crypto::NssValidity validity(CERT_CreateValidity(validFrom, validUntil));
    CERTCertificate* unsignedCertificate = request && validity
        ? CERT_CreateCertificate(QRandomGenerator::global()->generate() | 1U, subject, validity.get(), request.get())
        : nullptr;
    if (!unsignedCertificate) {
        ::Mu::Plugin::Crypto::deleteTokenKeypair(privateKeyHandle.get(), publicKeyHandle.get());
        setError(error, QStringLiteral("Could not construct the self-signed certificate"));
        return false;
    }
    CertificateHandle unsignedHandle(unsignedCertificate);
    if (!addSelfSignedExtensions(unsignedHandle.get(), error)) {
        ::Mu::Plugin::Crypto::deleteTokenKeypair(privateKeyHandle.get(), publicKeyHandle.get());
        return false;
    }
    return encodeSignAndImport(slot, unsignedHandle, privateKeyHandle, publicKeyHandle, nickname, error);
}

bool deleteCertificate(const QString& databasePath, const QString& nickname, QString* error)
{
    clearError(error);
    if (nickname.trimmed().isEmpty()) {
        setError(error, QStringLiteral("No certificate was selected"));
        return false;
    }
    if (!ensureCertificateDatabase(databasePath, DatabaseAccess::ReadWrite, error))
        return false;
    std::lock_guard<std::mutex> lock(Plugin::Crypto::Internal::nssMutex());
    CERTCertDBHandle* database = CERT_GetDefaultCertDB();
    CertificateHandle certificate = ::Mu::Plugin::Crypto::findCertificateByNickname(database, nickname);
    if (!certificate) {
        setError(error, QStringLiteral("The selected certificate was not found"));
        return false;
    }
    if (::Mu::Plugin::Crypto::deleteCertificateAndKeys(certificate.get()) != SECSuccess) {
        setError(error, nssError());
        return false;
    }
    return true;
}

} // namespace Mu::Plugin::Crypto::CertificateDatabase
