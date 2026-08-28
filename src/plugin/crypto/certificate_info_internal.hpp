// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CRYPTO_CERTIFICATE_INFO_INTERNAL_HPP
#define MUPDF_PLUGIN_CRYPTO_CERTIFICATE_INFO_INTERNAL_HPP

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#pragma pop_macro("slots")

#include <QList>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Crypto::Internal {

/// Copies NSS-owned certificate fields into a transport-safe model value.
[[nodiscard]] Model::Certificate describeCertificate(CERTCertificate* certificate);
/// Lists certificates that have an associated private signing key.
[[nodiscard]] QList<Model::Certificate> listSigningCertificates();

} // namespace Mu::Plugin::Crypto::Internal

#endif // MUPDF_PLUGIN_CRYPTO_CERTIFICATE_INFO_INTERNAL_HPP
