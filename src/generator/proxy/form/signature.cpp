// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/form/signature.hpp"

#include <QTimeZone>

#include <limits>

#include "generator/conversion/certificate.hpp"

#ifdef MUPDF_FORMFIELD_REMOTE_SIGNING
#include "plugin/util/signature_image.hpp"
#include "plugin/worker_client.hpp"
#include "shared/model/types.hpp"
#endif

namespace Mu::Generator::Proxy::Form {

Signature::Signature(int id, Model::SignatureField data, Plugin::WorkerClient* backend)
    : m_id(id)
    , m_data(std::move(data))
    , m_backend(backend)
{
}

Signature::~Signature()
{
    const std::lock_guard<std::mutex> lock(m_subscriptionMutex);
    m_subscriptions.clear();
}

Okular::NormalizedRect Signature::rect() const
{
    return Okular::NormalizedRect(m_data.left, m_data.top, m_data.right, m_data.bottom);
}

QString Signature::name() const
{
    return QString::fromStdString(m_data.partialName);
}

QString Signature::uiName() const
{
    return QString::fromStdString(m_data.partialName);
}

QString Signature::fullyQualifiedName() const
{
    return QString::fromStdString(m_data.fullyQualifiedName.empty() ? m_data.partialName : m_data.fullyQualifiedName);
}

bool Signature::isReadOnly() const
{
    return m_data.readOnly;
}

bool Signature::isVisible() const
{
    return m_data.visible;
}

namespace {

Okular::SignatureInfo::SignatureStatus signatureStatus(Model::SignatureStatus status)
{
    switch (status) {
    case Model::SignatureStatus::Valid:
        return Okular::SignatureInfo::SignatureValid;
    case Model::SignatureStatus::Invalid:
        return Okular::SignatureInfo::SignatureInvalid;
    case Model::SignatureStatus::DigestMismatch:
        return Okular::SignatureInfo::SignatureDigestMismatch;
    case Model::SignatureStatus::DecodingError:
        return Okular::SignatureInfo::SignatureDecodingError;
    case Model::SignatureStatus::GenericError:
        return Okular::SignatureInfo::SignatureGenericError;
    case Model::SignatureStatus::NotFound:
        return Okular::SignatureInfo::SignatureNotFound;
    case Model::SignatureStatus::NotVerified:
        return Okular::SignatureInfo::SignatureNotVerified;
    case Model::SignatureStatus::Unknown:
    default:
        return Okular::SignatureInfo::SignatureStatusUnknown;
    }
}

Okular::SignatureInfo::CertificateStatus certificateStatus(Model::CertificateStatus status)
{
    switch (status) {
    case Model::CertificateStatus::Trusted:
        return Okular::SignatureInfo::CertificateTrusted;
    case Model::CertificateStatus::UntrustedIssuer:
        return Okular::SignatureInfo::CertificateUntrustedIssuer;
    case Model::CertificateStatus::UnknownIssuer:
        return Okular::SignatureInfo::CertificateUnknownIssuer;
    case Model::CertificateStatus::Revoked:
        return Okular::SignatureInfo::CertificateRevoked;
    case Model::CertificateStatus::Expired:
        return Okular::SignatureInfo::CertificateExpired;
    case Model::CertificateStatus::GenericError:
        return Okular::SignatureInfo::CertificateGenericError;
    case Model::CertificateStatus::NotVerified:
        return Okular::SignatureInfo::CertificateNotVerified;
    case Model::CertificateStatus::VerificationInProgress:
        return Okular::SignatureInfo::CertificateVerificationInProgress;
    case Model::CertificateStatus::Unknown:
    default:
        return Okular::SignatureInfo::CertificateStatusUnknown;
    }
}

Okular::SignatureInfo::HashAlgorithm hashAlgorithm(Model::HashAlgorithm algorithm)
{
    switch (algorithm) {
    case Model::HashAlgorithm::Md2:
        return Okular::SignatureInfo::HashAlgorithmMd2;
    case Model::HashAlgorithm::Md5:
        return Okular::SignatureInfo::HashAlgorithmMd5;
    case Model::HashAlgorithm::Sha1:
        return Okular::SignatureInfo::HashAlgorithmSha1;
    case Model::HashAlgorithm::Sha256:
        return Okular::SignatureInfo::HashAlgorithmSha256;
    case Model::HashAlgorithm::Sha384:
        return Okular::SignatureInfo::HashAlgorithmSha384;
    case Model::HashAlgorithm::Sha512:
        return Okular::SignatureInfo::HashAlgorithmSha512;
    case Model::HashAlgorithm::Sha224:
        return Okular::SignatureInfo::HashAlgorithmSha224;
    case Model::HashAlgorithm::Unknown:
    default:
        return Okular::SignatureInfo::HashAlgorithmUnknown;
    }
}

bool appendRangeBounds(QList<qint64>& bounds, std::int64_t offset, std::int64_t length)
{
    // PDF byte ranges must be non-negative and may not overflow before being
    // converted to Okular's signed range representation.
    if (offset < 0 || length < 0 || length > std::numeric_limits<std::int64_t>::max() - offset)
        return false;
    bounds.append(offset);
    bounds.append(offset + length);
    return true;
}

} // namespace

Okular::FormFieldSignature::SignatureType Signature::signatureType() const
{
    if (!m_data.signedField)
        return Okular::FormFieldSignature::UnsignedSignature;
    if (m_data.subFilter == "adbe.pkcs7.sha1")
        return Okular::FormFieldSignature::AdbePkcs7sha1;
    if (m_data.subFilter == "adbe.pkcs7.detached")
        return Okular::FormFieldSignature::AdbePkcs7detached;
    if (m_data.subFilter == "ETSI.CAdES.detached")
        return Okular::FormFieldSignature::EtsiCAdESdetached;
    return Okular::FormFieldSignature::UnknownType;
}

Okular::SignatureInfo Signature::signatureInfo() const
{
    // Convert the worker's verification model once, preserving only validated
    // byte-range bounds for Okular's signature viewer.
    Okular::SignatureInfo info;
    info.setSignatureStatus(signatureStatus(m_data.signatureStatus));
    info.setCertificateStatus(certificateStatus(m_data.certificateStatus));
    info.setSignerName(QString::fromStdString(m_data.signerName));
    info.setSignerSubjectDN(QString::fromStdString(m_data.signerSubjectDn));
    info.setReason(QString::fromStdString(m_data.reason));
    info.setLocation(QString::fromStdString(m_data.location));
    if (m_data.signingTime.valid)
        info.setSigningTime(QDateTime::fromMSecsSinceEpoch(m_data.signingTime.unixMilliseconds, QTimeZone::UTC));
    info.setSignature(QByteArray(reinterpret_cast<const char*>(m_data.cmsSignature.data()),
                                 static_cast<qsizetype>(m_data.cmsSignature.size())));
    QList<qint64> rangeBounds;
    if (m_data.byteRange.size() == 4) {
        const bool valid = appendRangeBounds(rangeBounds, m_data.byteRange[0], m_data.byteRange[1])
            && appendRangeBounds(rangeBounds, m_data.byteRange[2], m_data.byteRange[3]);
        if (!valid)
            rangeBounds.clear();
    } else {
        for (const std::int64_t value : m_data.byteRange)
            rangeBounds.append(value);
    }
    info.setSignedRangeBounds(rangeBounds);
    info.setHashAlgorithm(hashAlgorithm(m_data.hashAlgorithm));
    info.setSignsTotalDocument(m_data.signsTotalDocument);
    info.setCertificateInfo(Conversion::toOkularCertificateInfo(m_data.certificate));
    return info;
}

std::pair<Okular::SigningResult, QString> Signature::sign(const Okular::NewSignatureData& data,
                                                          const QString& newPath) const
{
#ifndef MUPDF_FORMFIELD_REMOTE_SIGNING
    // Keep the feature compile-time gated; builds without remote signing still
    // expose verification information but cannot create a new signature.
    Q_UNUSED(data)
    Q_UNUSED(newPath)
    return { Okular::GenericSigningError, QStringLiteral("MuPDF worker support is unavailable") };
#else
    // Signing is delegated to the worker so private keys and PDF mutation stay
    // outside the generator process.
    if (!m_backend || !m_backend->isConnected()) {
        return { Okular::GenericSigningError, QStringLiteral("MuPDF worker is unavailable") };
    }
    const auto bgImage = Plugin::Util::SignatureImage::prepareBackgroundImage(
        data.backgroundImagePath(), rect().width(), rect().height());
    const auto result = m_backend->sign({ { },
                                          m_data.page,
                                          { },
                                          data.certNickname().toStdString(),
                                          data.certSubjectCommonName().toStdString(),
                                          data.reason().toStdString(),
                                          data.location().toStdString(),
                                          m_data.objectNumber,
                                          bgImage },
                                        data.password(),
                                        newPath);
    switch (result.result) {
    case Model::SigningResult::Success:
        return { Okular::SigningSuccess, QString() };
    case Model::SigningResult::FieldAlreadySigned:
        return { Okular::FieldAlreadySigned, QString::fromStdString(result.details) };
    case Model::SigningResult::KeyMissing:
        return { Okular::KeyMissing, QString::fromStdString(result.details) };
    case Model::SigningResult::WriteFailed:
        return { Okular::SignatureWriteFailed, QString::fromStdString(result.details) };
    case Model::SigningResult::UserCancelled:
        return { Okular::UserCancelled, QString::fromStdString(result.details) };
    case Model::SigningResult::BadPassphrase:
        return { Okular::BadPassphrase, QString::fromStdString(result.details) };
    default:
        return { Okular::GenericSigningError, QString::fromStdString(result.details) };
    }
#endif
}

Signature::SubscriptionHandle Signature::subscribeUpdates(const std::function<void()>& callback) const
{
    if (!callback)
        return 0;
    // Allocate handles under the same lock used for removal/destruction.
    const std::lock_guard<std::mutex> lock(m_subscriptionMutex);
    SubscriptionHandle handle = m_nextSubscriptionHandle++;
    m_subscriptions[handle] = callback;
    return handle;
}

bool Signature::unsubscribeUpdates(const SubscriptionHandle& handle) const
{
    // Missing handles are harmless and report that no subscription was removed.
    const std::lock_guard<std::mutex> lock(m_subscriptionMutex);
    auto it = m_subscriptions.find(handle);
    if (it != m_subscriptions.end()) {
        m_subscriptions.erase(it);
        return true;
    }
    return false;
}

} // namespace Mu::Generator::Proxy::Form
