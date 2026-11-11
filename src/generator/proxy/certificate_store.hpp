// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_CERTIFICATE_STORE_HPP
#define MU_GENERATOR_PROXY_CERTIFICATE_STORE_HPP

#include <okular/core/signatureutils.h>
#include <okular/core/version.h>

namespace Mu::Generator::Proxy {

class CertificateStore final : public Okular::CertificateStore {
public:
#if OKULAR_VERSION >= QT_VERSION_CHECK(26, 11, 70) // Parent-aware certificate store API.
    QList<Okular::CertificateInfo> signingCertificates(QWidget* parent, bool* userCancelled) const override;
#else
    QList<Okular::CertificateInfo> signingCertificates(bool* userCancelled) const override;
#endif
};

} // namespace Mu::Generator::Proxy

#endif // MU_GENERATOR_PROXY_CERTIFICATE_STORE_HPP
