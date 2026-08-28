// SPDX-License-Identifier: GPL-3.0-or-later

#include "certificate.hpp"

#include <QTimeZone>

namespace Mu::Generator::Conversion {

Okular::CertificateInfo toOkularCertificateInfo(const Model::Certificate& source)
{
    Okular::CertificateInfo result;
    result.setNull(source.null);
    result.setVersion(source.version);
    result.setSerialNumber(QByteArray(reinterpret_cast<const char*>(source.serialNumber.data()),
                                      static_cast<qsizetype>(source.serialNumber.size())));
    result.setIssuerInfo(Okular::CertificateInfo::CommonName, QString::fromStdString(source.issuerCommonName));
    result.setIssuerInfo(Okular::CertificateInfo::DistinguishedName,
                         QString::fromStdString(source.issuerDistinguishedName));
    result.setIssuerInfo(Okular::CertificateInfo::EmailAddress, QString::fromStdString(source.issuerEmail));
    result.setIssuerInfo(Okular::CertificateInfo::Organization, QString::fromStdString(source.issuerOrganization));
    result.setSubjectInfo(Okular::CertificateInfo::CommonName, QString::fromStdString(source.subjectCommonName));
    result.setSubjectInfo(Okular::CertificateInfo::DistinguishedName,
                          QString::fromStdString(source.subjectDistinguishedName));
    result.setSubjectInfo(Okular::CertificateInfo::EmailAddress, QString::fromStdString(source.subjectEmail));
    result.setSubjectInfo(Okular::CertificateInfo::Organization, QString::fromStdString(source.subjectOrganization));
    result.setNickName(QString::fromStdString(source.nickname));
    if (source.validityStart.valid)
        result.setValidityStart(QDateTime::fromMSecsSinceEpoch(source.validityStart.unixMilliseconds, QTimeZone::UTC));
    if (source.validityEnd.valid)
        result.setValidityEnd(QDateTime::fromMSecsSinceEpoch(source.validityEnd.unixMilliseconds, QTimeZone::UTC));
    result.setSelfSigned(source.selfSigned);
    result.setKeyUsageExtensions(static_cast<Okular::CertificateInfo::KeyUsageExtensions>(source.keyUsage));
    result.setPublicKey(QByteArray(reinterpret_cast<const char*>(source.publicKey.data()),
                                   static_cast<qsizetype>(source.publicKey.size())));
    result.setPublicKeyType(static_cast<Okular::CertificateInfo::PublicKeyType>(source.publicKeyType));
    result.setPublicKeyStrength(source.publicKeyStrength);
    result.setCertificateData(
        QByteArray(reinterpret_cast<const char*>(source.der.data()), static_cast<qsizetype>(source.der.size())));
    return result;
}

} // namespace Mu::Generator::Conversion
