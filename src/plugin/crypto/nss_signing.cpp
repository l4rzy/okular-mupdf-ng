// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/crypto/certificate_info_internal.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/nss_error_internal.hpp"
#include "plugin/crypto/nss_handles.hpp"
#include "plugin/crypto/nss_internal.hpp"

#include <array>

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <cms.h>
#include <pk11pub.h>
#include <prerror.h>
#pragma pop_macro("slots")

namespace Mu::Plugin::Crypto {

using namespace ::Mu::Model;

namespace {

bool authenticateSigningKey(CERTCertificate* cert, const QString& password)
{
    // NSS may already have an authenticated slot. Otherwise authenticate with
    // the supplied password and wipe its UTF-8 copy before returning.
    if (!cert)
        return false;
    NssPrivateKey key(PK11_FindKeyByAnyCert(cert, nullptr));
    if (!key)
        return false;
    NssSlot slot(PK11_GetSlotFromPrivateKey(key.get()));
    if (!slot)
        return false;
    bool authenticated = PK11_IsLoggedIn(slot.get(), nullptr);
    SensitiveBytes passwordBytes(password.toUtf8());
    if (!authenticated) {
        authenticated = passwordBytes.bytes.isEmpty()
            ? PK11_Authenticate(slot.get(), PR_TRUE, nullptr) == SECSuccess
            : PK11_CheckUserPassword(slot.get(), passwordBytes.bytes.constData()) == SECSuccess;
    }
    return authenticated;
}

CmsResult signingFailure(const QString& operation)
{
    // NSS stores the detailed failure in thread-local state; capture it before
    // another NSS call replaces the error code.
    const PRErrorCode error = PR_GetError();
    return { SigningResult::GenericError,
             QStringLiteral("%1: %2").arg(operation, Internal::nssErrorMessage(error)),
             { } };
}

} // namespace

CmsResult createDetachedCmsFromDigest(const QString& certNickname,
                                      const QString& password,
                                      const std::array<std::uint8_t, 32>& digest)
{
    // Build a detached CMS object around a caller-computed digest. The RAII
    // wrappers below release NSS objects on every construction failure.
    if (!ensureNssInitialized())
        return { SigningResult::GenericError, QStringLiteral("NSS is unavailable"), { } };
    CERTCertDBHandle* certdb = CERT_GetDefaultCertDB();
    if (!certdb)
        return { SigningResult::GenericError, QStringLiteral("NSS certificate database is unavailable"), { } };
    NssCertificate cert = findCertificateByNickname(certdb, certNickname);
    if (!cert)
        return { SigningResult::KeyMissing, QStringLiteral("Signing certificate was not found"), { } };
    if (!authenticateSigningKey(cert.get(), password))
        return { SigningResult::BadPassphrase, QStringLiteral("Signing key authentication failed"), { } };
    auto digestCopy = digest;
    // NSS consumes this temporary SECItem during message construction; keep
    // the copied digest alive for the complete operation.
    SECItem digestItem { siBuffer, digestCopy.data(), static_cast<unsigned int>(digestCopy.size()) };
    NssCmsMessage message(NSS_CMSMessage_Create(nullptr));
    if (!message)
        return signingFailure(QStringLiteral("Could not create CMS message"));
    NSSCMSSignedData* signedData = NSS_CMSSignedData_Create(message.get());
    if (!signedData)
        return signingFailure(QStringLiteral("Could not create CMS signed-data content"));
    NSSCMSContentInfo* content = NSS_CMSMessage_GetContentInfo(message.get());
    if (!content)
        return signingFailure(QStringLiteral("Could not get CMS content info"));
    if (NSS_CMSContentInfo_SetContent_SignedData(message.get(), content, signedData) != SECSuccess)
        return signingFailure(QStringLiteral("Could not attach CMS signed-data content"));
    content = NSS_CMSSignedData_GetContentInfo(signedData);
    if (!content)
        return signingFailure(QStringLiteral("Could not get CMS signed-data content info"));
    if (NSS_CMSContentInfo_SetContent_Data(message.get(), content, nullptr, PR_TRUE) != SECSuccess)
        return signingFailure(QStringLiteral("Could not configure CMS detached content"));
    NSSCMSSignerInfo* signer = NSS_CMSSignerInfo_Create(message.get(), cert.get(), SEC_OID_SHA256);
    if (!signer)
        return signingFailure(QStringLiteral("Could not create CMS signer information"));
    if (NSS_CMSSignerInfo_IncludeCerts(signer, NSSCMSCM_CertChain, Internal::PdfCmsCertUsage) != SECSuccess)
        return signingFailure(QStringLiteral("Could not include CMS certificate chain"));
    if (NSS_CMSSignedData_AddSignerInfo(signedData, signer) != SECSuccess)
        return signingFailure(QStringLiteral("Could not add CMS signer information"));
    if (NSS_CMSSignedData_SetDigestValue(signedData, SEC_OID_SHA256, &digestItem) != SECSuccess)
        return signingFailure(QStringLiteral("Could not set CMS digest value"));
    SECItem encoded { siBuffer, nullptr, 0 };
    NssArena arena(PORT_NewArena(16384));
    if (!arena)
        return signingFailure(QStringLiteral("Could not allocate CMS encoder arena"));
    NssCmsEncoder encoder(NSS_CMSEncoder_Start(
        message.get(), nullptr, nullptr, &encoded, arena.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!encoder)
        return signingFailure(QStringLiteral("Could not start CMS encoder"));
    const SECStatus finishStatus = NSS_CMSEncoder_Finish(encoder.get());
    encoder.release();
    if (finishStatus != SECSuccess)
        return signingFailure(QStringLiteral("Could not finish CMS encoder"));
    if (!encoded.data || !encoded.len)
        return signingFailure(QStringLiteral("CMS encoder produced no output"));
    QByteArray result(reinterpret_cast<const char*>(encoded.data), encoded.len);
    return { SigningResult::Success, { }, result };
}

bool checkSigningCertificatePassword(const QString& certNickname, const QString& password)
{
    // This is a validation-only operation; it does not create or persist a
    // signing result.
    if (!ensureNssInitialized() || certNickname.isEmpty())
        return false;
    CERTCertDBHandle* certdb = CERT_GetDefaultCertDB();
    NssCertificate cert = findCertificateByNickname(certdb, certNickname);
    return cert && authenticateSigningKey(cert.get(), password);
}

QList<Certificate> signingCertificates()
{
    // Return copied model data, never NSS-owned certificate handles.
    if (!ensureNssInitialized())
        return { };
    return Internal::listSigningCertificates();
}

QString signingCertificateCommonName(const QString& certNickname)
{
    // Prefer the readable Common Name, but retain a useful identity when a
    // certificate has only a subject DN or an incomplete subject.
    if (!ensureNssInitialized() || certNickname.isEmpty())
        return { };
    CERTCertDBHandle* certdb = CERT_GetDefaultCertDB();
    NssCertificate cert = findCertificateByNickname(certdb, certNickname);
    if (!cert)
        return { };
    const QString commonName = extractNssQString(cert->subject, CERT_GetCommonName);
    if (!commonName.isEmpty())
        return commonName;
    const QString asciiDn = extractNssQString(cert->subject, CERT_NameToAscii);
    if (!asciiDn.isEmpty())
        return asciiDn;
    return certNickname;
}

} // namespace Mu::Plugin::Crypto
