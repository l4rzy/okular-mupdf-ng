// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef QMUPDF_CERTIFICATE_STORE_HPP
#define QMUPDF_CERTIFICATE_STORE_HPP

#include <okular/core/signatureutils.h>

namespace Mu::Generator::Proxy {

class CertificateStore final : public Okular::CertificateStore {
public:
    QList<Okular::CertificateInfo> signingCertificates(bool* userCancelled) const override;
};

} // namespace Mu::Generator::Proxy

#endif
