// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/certificate_store.hpp"

#include "generator/conversion/certificate.hpp"
#include "plugin/crypto/nss.hpp"

namespace Mu::Generator::Proxy {

#if OKULAR_VERSION >= QT_VERSION_CHECK(26, 11, 70)
QList<Okular::CertificateInfo> CertificateStore::signingCertificates(QWidget*, bool* userCancelled) const
#else
QList<Okular::CertificateInfo> CertificateStore::signingCertificates(bool* userCancelled) const
#endif
{
    if (userCancelled)
        *userCancelled = false;
    QList<Okular::CertificateInfo> result;
    for (const Model::Certificate& source : Plugin::Crypto::signingCertificates()) {
        Okular::CertificateInfo certificate = Conversion::toOkularCertificateInfo(source);
        certificate.setNickName(QString::fromStdString(source.nickname));
        const QString nickname = certificate.nickName();
        certificate.setCheckPasswordFunction([nickname](const QString& password) {
            return Plugin::Crypto::checkSigningCertificatePassword(nickname, password);
        });
        result.append(certificate);
    }
    return result;
}

} // namespace Mu::Generator::Proxy
