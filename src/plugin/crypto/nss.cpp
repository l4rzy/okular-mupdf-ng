// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/certificate_info_internal.hpp"
#include "plugin/crypto/nss_handles.hpp"
#include "plugin/crypto/nss_internal.hpp"

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <certdb.h>
#include <cms.h>
#include <pk11pub.h>
#include <prerror.h>
#include <prtime.h>
#include <secerr.h>
#pragma pop_macro("slots")

#include <limits>

#include "shared/logging.hpp"

namespace Mu::Plugin::Crypto {

using namespace ::Mu::Model;

namespace {

struct ParsedCmsContext {
    NssCmsMessage cmsg;
    NSSCMSSignedData* signedData = nullptr;
    NSSCMSSignerInfo* signerInfo = nullptr;

    static ParsedCmsContext parse(unsigned char* signature, size_t len)
    {
        // Parse into an RAII-owned CMS message and retain only pointers whose
        // lifetime is covered by cmsg. Callers must keep this context alive
        // through verification.
        ParsedCmsContext res;
        if (!signature || len == 0 || len > std::numeric_limits<unsigned int>::max())
            return res;
        if (!ensureNssInitialized())
            return res;

        SECItem sigItem = { siBuffer, signature, static_cast<unsigned int>(len) };
        res.cmsg.reset(NSS_CMSMessage_CreateFromDER(&sigItem, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        if (!res.cmsg)
            return res;

        NSSCMSContentInfo* cinfo = NSS_CMSMessage_GetContentInfo(res.cmsg.get());
        if (!cinfo || NSS_CMSContentInfo_GetContentTypeTag(cinfo) != SEC_OID_PKCS7_SIGNED_DATA) {
            return res;
        }

        res.signedData = static_cast<NSSCMSSignedData*>(NSS_CMSContentInfo_GetContent(cinfo));
        if (!res.signedData)
            return res;

        res.signerInfo = NSS_CMSSignedData_GetSignerInfo(res.signedData, 0);
        return res;
    }

    explicit operator bool() const { return signerInfo != nullptr; }
};

// Make CMS-provided certificates available to NSS only for this verification
// operation. Temporary certificates are not written to the user's DB.
NssCertificate
loadCmsSignerCertificate(NSSCMSSignedData* signedData, NSSCMSSignerInfo* signerInfo, CERTCertDBHandle* certdb)
{
    NssCertificate result;
    if (!signedData || !certdb)
        return result;

    if (NSS_CMSSignedData_ImportCerts(signedData, certdb, Internal::PdfCmsCertUsage, PR_FALSE) != SECSuccess) {
        MU_LOG(warning,
               "Mu::Plugin::Crypto",
               std::string("could not import certificates embedded in PDF signature error=")
                   + std::to_string(PR_GetError()));
        return result;
    }

    if (!signerInfo)
        return result;
    // NSS returns a reference owned by the caller here. Keep that reference
    // alive for the complete validation operation and release it exactly once.
    CERTCertificate* signer = NSS_CMSSignerInfo_GetSigningCertificate(signerInfo, certdb);
    if (signer)
        result.reset(signer);
    else
        MU_LOG(warning,
               "Mu::Plugin::Crypto",
               std::string("could not resolve PDF signature signer certificate error=")
                   + std::to_string(PR_GetError()));
    return result;
}

bool isTrustedForEmailSigning(const CERTCertificate* certificate)
{
    // Email-signing trust is the relevant NSS trust bit for PDF signatures;
    // CA trust alone does not make an end-entity signer trusted here.
    CERTCertTrust trust { };
    if (!certificate || CERT_GetCertTrust(certificate, &trust) != SECSuccess)
        return false;
    return (trust.emailFlags & (CERTDB_TRUSTED | CERTDB_TRUSTED_CA)) != 0;
}

CertificateStatus certificateStatusAt(CERTCertDBHandle* certdb, CERTCertificate* certificate, PRTime validationTime)
{
    // Report time validity separately from chain trust so callers can explain
    // expired, revoked, self-signed, and unknown-issuer certificates distinctly.
    if (!certificate)
        return CertificateStatus::UnknownIssuer;
    switch (CERT_CheckCertValidTimes(certificate, validationTime, PR_FALSE)) {
    case secCertTimeExpired:
        return CertificateStatus::Expired;
    case secCertTimeNotValidYet:
        return CertificateStatus::UnknownIssuer;
    default:
        break;
    }
    // NSS 3.126 can recurse indefinitely while building a chain for some
    // self-signed CMS certificates. A self-signed certificate has no issuer
    // chain to validate; use its explicit email-signing trust state instead.
    if (isSelfSigned(certificate))
        return isTrustedForEmailSigning(certificate) ? CertificateStatus::Trusted : CertificateStatus::UntrustedIssuer;
    SECCertificateUsage returnedUsages = 0;
    if (certdb
        && CERT_VerifyCertificate(certdb,
                                  certificate,
                                  PR_TRUE,
                                  Internal::PdfCertificateVerificationUsage,
                                  validationTime,
                                  nullptr,
                                  nullptr,
                                  &returnedUsages)
            == SECSuccess)
        return CertificateStatus::Trusted;
    const PRErrorCode error = PR_GetError();
    if (error == SEC_ERROR_REVOKED_CERTIFICATE)
        return CertificateStatus::Revoked;
    if (error == SEC_ERROR_UNTRUSTED_ISSUER || error == SEC_ERROR_UNKNOWN_ISSUER || error == SEC_ERROR_UNTRUSTED_CERT
        || error == SEC_ERROR_CA_CERT_INVALID)
        return CertificateStatus::UntrustedIssuer;
    if (error == SEC_ERROR_EXPIRED_CERTIFICATE)
        return CertificateStatus::Expired;
    return CertificateStatus::UnknownIssuer;
}

void resetValidationState(SignatureField& field)
{
    // Signature validation is repeatable; clear all derived state before
    // attempting to parse the new untrusted input.
    field.certificate = { };
    field.signerName.clear();
    field.signerSubjectDn.clear();
    field.signatureStatus = SignatureStatus::GenericError;
    field.certificateStatus = CertificateStatus::NotVerified;
    field.certificateStatusAtSigningTime = CertificateStatus::NotVerified;
    field.certificateStatusCurrent = CertificateStatus::NotVerified;
    field.signingTime = { };
    field.certificateValidationTime = { };
    field.hasTrustedTimestamp = false;
    field.hashAlgorithm = HashAlgorithm::Unknown;
    field.signsTotalDocument = false;
}

bool hashByteRange(QIODevice& source, PK11Context* hash, qint64 offset, qint64 length)
{
    // Hash in bounded chunks so a large document cannot force one large
    // allocation merely to validate a detached signature.
    if (!hash || !source.seek(offset))
        return false;
    while (length > 0) {
        const QByteArray bytes = source.read(qMin<qint64>(length, 64 * 1024));
        if (bytes.isEmpty()
            || PK11_DigestOp(hash,
                             reinterpret_cast<const unsigned char*>(bytes.constData()),
                             static_cast<unsigned int>(bytes.size()))
                != SECSuccess)
            return false;
        length -= bytes.size();
    }
    return true;
}

bool parseAndValidateByteRange(const SignatureField& field,
                               qint64 sourceSize,
                               qint64* firstOffset,
                               qint64* firstLength,
                               qint64* secondOffset,
                               qint64* secondLength)
{
    // Validate ranges before seeking so malformed PDF data cannot cause an
    // out-of-bounds read or an integer calculation outside the source.
    if (field.byteRange.size() != 4)
        return false;
    *firstOffset = field.byteRange[0];
    *firstLength = field.byteRange[1];
    *secondOffset = field.byteRange[2];
    *secondLength = field.byteRange[3];
    if (*firstOffset != 0 || *firstLength < 0 || *secondOffset < *firstLength || *secondLength < 0
        || *secondOffset > sourceSize || *secondLength > sourceSize - *secondOffset)
        return false;
    return true;
}

} // namespace

HashAlgorithm Internal::hashAlgorithmForDigest(SECOidTag digestAlgorithm)
{
    switch (digestAlgorithm) {
    case SEC_OID_MD5:
        return HashAlgorithm::Md5;
    case SEC_OID_SHA1:
        return HashAlgorithm::Sha1;
    case SEC_OID_SHA224:
        return HashAlgorithm::Sha224;
    case SEC_OID_SHA256:
        return HashAlgorithm::Sha256;
    case SEC_OID_SHA384:
        return HashAlgorithm::Sha384;
    case SEC_OID_SHA512:
        return HashAlgorithm::Sha512;
    // SEC_OID_MD2 intentionally maps to Unknown – MD2 is deprecated and not used for PDF signatures.
    default:
        return HashAlgorithm::Unknown;
    }
}

void validateDetachedPdfSignature(SignatureField& field, QIODevice& source)
{
    // Validation proceeds in phases: validate the signed range, parse CMS,
    // hash the covered bytes, verify the signature, then classify the signer.
    const auto fallbackSigningTime = field.signingTime;
    resetValidationState(field);
    if (!field.signedField)
        return;
    if (field.byteRange.size() != 4) {
        field.signatureStatus = SignatureStatus::DecodingError;
        return;
    }
    if (!source.isOpen()) {
        field.signatureStatus = SignatureStatus::GenericError;
        return;
    }
    if (field.cmsSignature.empty()) {
        field.signatureStatus = SignatureStatus::DecodingError;
        return;
    }
    QIODevicePositionGuard restorePosition(source);

    const qint64 size = source.size();
    if (size < 0) {
        field.signatureStatus = SignatureStatus::GenericError;
        return;
    }
    qint64 firstOffset = 0, firstLength = 0, secondOffset = 0, secondLength = 0;
    if (!parseAndValidateByteRange(field, size, &firstOffset, &firstLength, &secondOffset, &secondLength)) {
        field.signatureStatus = SignatureStatus::DecodingError;
        return;
    }
    field.signsTotalDocument = secondOffset + secondLength == size;
    auto cms = ParsedCmsContext::parse(field.cmsSignature.data(), field.cmsSignature.size());
    if (!cms) {
        field.signatureStatus = SignatureStatus::DecodingError;
        return;
    }
    CERTCertDBHandle* certdb = CERT_GetDefaultCertDB();
    const NssCertificate certificate = loadCmsSignerCertificate(cms.signedData, cms.signerInfo, certdb);
    if (!certificate) {
        field.signatureStatus = SignatureStatus::DecodingError;
        return;
    }
    field.certificate = Internal::describeCertificate(certificate.get());
    field.signerName = field.certificate.subjectCommonName;
    field.signerSubjectDn = field.certificate.subjectDistinguishedName;
    field.hashAlgorithm = Internal::hashAlgorithmForDigest(NSS_CMSSignerInfo_GetDigestAlgTag(cms.signerInfo));
    if (field.hashAlgorithm == HashAlgorithm::Unknown) {
        field.signatureStatus = SignatureStatus::DecodingError;
        return;
    }
    NssHashContext hash(PK11_CreateDigestContext(NSS_CMSSignerInfo_GetDigestAlgTag(cms.signerInfo)));
    if (!hash || PK11_DigestBegin(hash.get()) != SECSuccess) {
        field.signatureStatus = SignatureStatus::GenericError;
        return;
    }
    if (!hashByteRange(source, hash.get(), firstOffset, firstLength)
        || !hashByteRange(source, hash.get(), secondOffset, secondLength)) {
        field.signatureStatus = SignatureStatus::GenericError;
        return;
    }
    unsigned char digest[64];
    unsigned int digestLength = 0;
    if (PK11_DigestFinal(hash.get(), digest, &digestLength, sizeof(digest)) != SECSuccess) {
        field.signatureStatus = SignatureStatus::GenericError;
        return;
    }
    SECItem digestItem = { siBuffer, digest, digestLength };
    // CMS verifies the digest against the signed attributes; certificate trust
    // is evaluated separately below using the current validation time.
    const SECStatus verification = NSS_CMSSignerInfo_Verify(cms.signerInfo, &digestItem, nullptr);
    if (verification == SECSuccess) {
        field.signatureStatus = SignatureStatus::Valid;
    } else {
        switch (NSS_CMSSignerInfo_GetVerificationStatus(cms.signerInfo)) {
        case NSSCMSVS_BadSignature:
            field.signatureStatus = SignatureStatus::Invalid;
            break;
        case NSSCMSVS_DigestMismatch:
            field.signatureStatus = SignatureStatus::DigestMismatch;
            break;
        case NSSCMSVS_MalformedSignature:
            field.signatureStatus = SignatureStatus::DecodingError;
            break;
        default:
            field.signatureStatus = SignatureStatus::GenericError;
            break;
        }
    }
    PRTime signingTime = 0;
    const bool hasSignedTime = NSS_CMSSignerInfo_GetSigningTime(cms.signerInfo, &signingTime) == SECSuccess;
    const PRTime currentTime = PR_Now();
    field.certificateValidationTime = { true, currentTime / 1000 };
    if (hasSignedTime)
        field.signingTime = { true, signingTime / 1000 };
    else if (fallbackSigningTime.valid)
        field.signingTime = fallbackSigningTime;
    // CMS signingTime is signed by the signer but is not an RFC 3161 trusted
    // timestamp. Without an independently authenticated timestamp token, historical
    // trust cannot be verified; validate at the current evaluation time.
    field.hasTrustedTimestamp = false;
    field.certificateStatusCurrent = certificateStatusAt(certdb, certificate.get(), currentTime);
    field.certificateStatusAtSigningTime = field.certificateStatusCurrent;
    field.certificateStatus = field.certificateStatusCurrent;
}

} // namespace Mu::Plugin::Crypto
