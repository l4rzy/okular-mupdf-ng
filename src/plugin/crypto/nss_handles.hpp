// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CRYPTO_NSS_HANDLES_HPP
#define MUPDF_PLUGIN_CRYPTO_NSS_HANDLES_HPP

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <certdb.h>
#include <cms.h>
#include <keyhi.h>
#include <p12.h>
#include <pk11pub.h>
#include <secport.h>
#pragma pop_macro("slots")

#include <QByteArray>
#include <QIODevice>
#include <QString>

#include <memory>
#include <string>
#include <utility>

namespace Mu::Plugin::Crypto {

/// Copies an NSS-allocated string and releases it with the NSS allocator.
[[nodiscard]]
inline std::string takeNssString(char* value)
{
    if (!value)
        return { };
    std::string result = value;
    PORT_Free(value);
    return result;
}

/// Copies an NSS-allocated UTF-8 string into Qt storage and releases it.
[[nodiscard]]
inline QString takeNssQString(char* value)
{
    if (!value)
        return { };
    QString result = QString::fromUtf8(value);
    PORT_Free(value);
    return result;
}

/// Applies an NSS name extractor and returns an owned standard string.
template <typename Extractor> [[nodiscard]] std::string extractNssString(CERTName& name, Extractor extractor)
{
    return takeNssString(extractor(&name));
}

/// Applies an NSS name extractor and returns an owned Qt string.
template <typename Extractor> [[nodiscard]] QString extractNssQString(CERTName& name, Extractor extractor)
{
    return takeNssQString(extractor(&name));
}

/// Owns byte storage that is wiped when the object is destroyed.
class SensitiveBytes {
public:
    explicit SensitiveBytes(QByteArray value)
        : bytes(std::move(value))
    {
    }

    ~SensitiveBytes() { bytes.fill('\0'); }

    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;
    /// Moving transfers the buffer to a new owner; move assignment is forbidden because it could skip wiping.
    SensitiveBytes(SensitiveBytes&&) noexcept = default;
    SensitiveBytes& operator=(SensitiveBytes&&) = delete;

    QByteArray bytes;
};

/// Releases an NSS certificate reference.
struct CertificateDeleter {
    void operator()(CERTCertificate* value) const { CERT_DestroyCertificate(value); }
};

/// Releases an NSS private-key reference.
struct PrivateKeyDeleter {
    void operator()(SECKEYPrivateKey* value) const { SECKEY_DestroyPrivateKey(value); }
};

/// Releases an NSS public-key reference.
struct PublicKeyDeleter {
    void operator()(SECKEYPublicKey* value) const { SECKEY_DestroyPublicKey(value); }
};

/// Releases an NSS subject-public-key-info object.
struct SubjectPublicKeyInfoDeleter {
    void operator()(CERTSubjectPublicKeyInfo* value) const { SECKEY_DestroySubjectPublicKeyInfo(value); }
};

/// Releases an NSS certificate-request object.
struct CertificateRequestDeleter {
    void operator()(CERTCertificateRequest* value) const { CERT_DestroyCertificateRequest(value); }
};

/// Releases an NSS validity object.
struct ValidityDeleter {
    void operator()(CERTValidity* value) const { CERT_DestroyValidity(value); }
};

/// Releases an NSS slot reference.
struct SlotDeleter {
    void operator()(PK11SlotInfo* value) const { PK11_FreeSlot(value); }
};

/// Releases an NSS certificate-list object.
struct CertificateListDeleter {
    void operator()(CERTCertList* value) const { CERT_DestroyCertList(value); }
};

/// Releases an NSS CMS message.
struct CmsMessageDeleter {
    void operator()(NSSCMSMessage* value) const { NSS_CMSMessage_Destroy(value); }
};

/// Destroys an NSS digest context and its sensitive state.
struct HashContextDeleter {
    void operator()(PK11Context* value) const { PK11_DestroyContext(value, PR_TRUE); }
};

/// Finishes an NSS PKCS#12 decoder during scope unwinding.
struct Pkcs12DecoderDeleter {
    void operator()(SEC_PKCS12DecoderContext* value) const { SEC_PKCS12DecoderFinish(value); }
};

/// Releases an NSS arena and its contents.
struct ArenaDeleter {
    void operator()(PLArenaPool* value) const { PORT_FreeArena(value, PR_TRUE); }
};

/// Cancels an unfinished NSS CMS encoder.
struct CmsEncoderDeleter {
    void operator()(NSSCMSEncoderContext* value) const
    {
        if (value)
            NSS_CMSEncoder_Cancel(value);
    }
};

using NssCertificate = std::unique_ptr<CERTCertificate, CertificateDeleter>;
using NssPrivateKey = std::unique_ptr<SECKEYPrivateKey, PrivateKeyDeleter>;
using NssPublicKey = std::unique_ptr<SECKEYPublicKey, PublicKeyDeleter>;
using NssSubjectPublicKeyInfo = std::unique_ptr<CERTSubjectPublicKeyInfo, SubjectPublicKeyInfoDeleter>;
using NssCertificateRequest = std::unique_ptr<CERTCertificateRequest, CertificateRequestDeleter>;
using NssValidity = std::unique_ptr<CERTValidity, ValidityDeleter>;
using NssSlot = std::unique_ptr<PK11SlotInfo, SlotDeleter>;
using NssCertificateList = std::unique_ptr<CERTCertList, CertificateListDeleter>;
using NssPkcs12Decoder = std::unique_ptr<SEC_PKCS12DecoderContext, Pkcs12DecoderDeleter>;
using NssArena = std::unique_ptr<PLArenaPool, ArenaDeleter>;
using NssCmsMessage = std::unique_ptr<NSSCMSMessage, CmsMessageDeleter>;
using NssHashContext = std::unique_ptr<PK11Context, HashContextDeleter>;
using NssCmsEncoder = std::unique_ptr<NSSCMSEncoderContext, CmsEncoderDeleter>;

/// Reports whether the certificate subject and issuer names match.
[[nodiscard]]
inline bool isSelfSigned(const CERTCertificate* certificate)
{
    return certificate && CERT_CompareName(&certificate->subject, &certificate->issuer) == SECEqual;
}

/// Finds a certificate by nickname and returns an owned NSS reference.
[[nodiscard]]
inline NssCertificate findCertificateByNickname(CERTCertDBHandle* certdb, const QString& nickname)
{
    if (!certdb || nickname.isEmpty())
        return NssCertificate(nullptr);
    const QByteArray name = nickname.toUtf8();
    return NssCertificate(CERT_FindCertByNickname(certdb, name.constData()));
}

/// Deletes persistent private and public key objects after a failed import.
inline void deleteTokenKeypair(SECKEYPrivateKey* priv, SECKEYPublicKey* pub)
{
    if (priv)
        PK11_DeleteTokenPrivateKey(priv, PR_TRUE);
    if (pub)
        PK11_DeleteTokenPublicKey(pub);
}

/// Restores a readable device to its original position when leaving scope.
struct QIODevicePositionGuard {
    QIODevice& source;
    qint64 position;

    explicit QIODevicePositionGuard(QIODevice& device)
        : source(device)
        , position(device.pos())
    {
    }

    ~QIODevicePositionGuard()
    {
        if (position >= 0)
            source.seek(position);
    }

    QIODevicePositionGuard(const QIODevicePositionGuard&) = delete;
    QIODevicePositionGuard& operator=(const QIODevicePositionGuard&) = delete;
};

} // namespace Mu::Plugin::Crypto

#endif // MUPDF_PLUGIN_CRYPTO_NSS_HANDLES_HPP
