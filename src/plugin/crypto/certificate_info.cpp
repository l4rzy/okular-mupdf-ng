// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/crypto/certificate_info_internal.hpp"
#include "plugin/crypto/nss_handles.hpp"

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <keyhi.h>
#include <pk11pub.h>
#include <secder.h>
#pragma pop_macro("slots")

#include <cstdint>
#include <string>
#include <utility>

namespace Mu::Plugin::Crypto::Internal {

using namespace ::Mu::Model;

namespace {

template <typename Extractor> std::string extractNameValue(CERTName& name, Extractor extractor)
{
    // Keep NSS allocation and release in one helper so copied model data never
    // outlives the certificate handle that supplied it.
    return Plugin::Crypto::extractNssString(name, extractor);
}

void describeName(CERTName& name,
                  std::string& commonName,
                  std::string& distinguishedName,
                  std::string& email,
                  std::string& organization)
{
    // Copy all display forms while the certificate remains owned by NSS.
    commonName = extractNameValue(name, CERT_GetCommonName);
    distinguishedName = extractNameValue(name, CERT_NameToAscii);
    email = extractNameValue(name, CERT_GetCertEmailAddress);
    organization = extractNameValue(name, CERT_GetOrgName);
}

} // namespace

Certificate describeCertificate(CERTCertificate* cert)
{
    // Convert the NSS certificate into a self-contained model before the
    // caller releases its reference-counted handle.
    Certificate result;
    if (!cert)
        return result;
    result.null = false;
    result.nickname = cert->nickname ? cert->nickname : "";

    describeName(cert->issuer,
                 result.issuerCommonName,
                 result.issuerDistinguishedName,
                 result.issuerEmail,
                 result.issuerOrganization);
    describeName(cert->subject,
                 result.subjectCommonName,
                 result.subjectDistinguishedName,
                 result.subjectEmail,
                 result.subjectOrganization);

    result.version = cert->version.len > 0 ? static_cast<int>(DER_GetInteger(&cert->version)) + 1 : 1;
    if (cert->serialNumber.data && cert->serialNumber.len > 0)
        result.serialNumber.assign(cert->serialNumber.data, cert->serialNumber.data + cert->serialNumber.len);

    PRTime notBefore = 0;
    if (DER_DecodeTimeChoice(&notBefore, &cert->validity.notBefore) == SECSuccess)
        result.validityStart = { true, notBefore / 1000 };
    PRTime notAfter = 0;
    if (DER_DecodeTimeChoice(&notAfter, &cert->validity.notAfter) == SECSuccess)
        result.validityEnd = { true, notAfter / 1000 };

    constexpr std::pair<unsigned int, uint32_t> keyUsageMap[] = {
        { KU_DIGITAL_SIGNATURE, 0x80 }, { KU_NON_REPUDIATION, 0x40 }, { KU_KEY_ENCIPHERMENT, 0x20 },
        { KU_DATA_ENCIPHERMENT, 0x10 }, { KU_KEY_AGREEMENT, 0x08 },   { KU_KEY_CERT_SIGN, 0x04 },
        { KU_CRL_SIGN, 0x02 },          { KU_ENCIPHER_ONLY, 0x01 },
        // KU_DECIPHER_ONLY intentionally not mapped – requires KU_KEY_AGREEMENT and is not used for signing.
    };
    uint32_t keyUsage = 0;
    for (const auto& [flag, bit] : keyUsageMap) {
        if (cert->keyUsage & flag)
            keyUsage |= bit;
    }
    result.keyUsage = keyUsage;

    if (cert->derPublicKey.data && cert->derPublicKey.len > 0)
        result.publicKey.assign(cert->derPublicKey.data, cert->derPublicKey.data + cert->derPublicKey.len);
    NssPublicKey publicKey(CERT_ExtractPublicKey(cert));
    if (publicKey) {
        switch (publicKey->keyType) {
        case rsaKey:
            result.publicKeyType = 0;
            break;
        case dsaKey:
            result.publicKeyType = 1;
            break;
        case ecKey:
            result.publicKeyType = 2;
            break;
        default:
            result.publicKeyType = 3;
            break;
        }
        result.publicKeyStrength = static_cast<int>(SECKEY_PublicKeyStrengthInBits(publicKey.get()));
    }
    result.selfSigned = isSelfSigned(cert);
    if (cert->derCert.data && cert->derCert.len > 0)
        result.der.assign(cert->derCert.data, cert->derCert.data + cert->derCert.len);
    return result;
}

QList<Certificate> listSigningCertificates()
{
    // Enumerate only certificates that have a matching private key; such
    // entries are the ones the signing UI can actually use.
    QList<Certificate> result;
    NssCertificateList certificates(PK11_ListCerts(PK11CertListUnique, nullptr));
    if (!certificates)
        return result;
    for (CERTCertListNode* node = CERT_LIST_HEAD(certificates.get()); !CERT_LIST_END(node, certificates.get());
         node = CERT_LIST_NEXT(node)) {
        if (!node->cert)
            continue;
        NssPrivateKey key(PK11_FindKeyByAnyCert(node->cert, nullptr));
        if (!key)
            continue;
        result.append(describeCertificate(node->cert));
    }
    return result;
}

} // namespace Mu::Plugin::Crypto::Internal
