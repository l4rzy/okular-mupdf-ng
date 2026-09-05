// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CRYPTO_NSS_INTERNAL_HPP
#define MU_PLUGIN_CRYPTO_NSS_INTERNAL_HPP

#pragma push_macro("slots")
#undef slots
#include <certt.h>
#include <secoidt.h>
#pragma pop_macro("slots")

#include <mutex>

#include <QString>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Crypto::Internal {

/// PDF signatures follow NSS's S/MIME email-signing policy. CMS APIs consume
/// SECCertUsage, while certificate verification consumes a bitmask type.
inline constexpr SECCertUsage PdfCmsCertUsage = certUsageEmailSigner;
inline constexpr SECCertificateUsage PdfCertificateVerificationUsage = certificateUsageEmailSigner;

/// Returns the mutex protecting process-global NSS state.
std::mutex& nssMutex();
/// Maps an NSS digest identifier to the transport model's hash enumeration.
[[nodiscard]] Model::HashAlgorithm hashAlgorithmForDigest(SECOidTag digestAlgorithm);
/// Removes a supported NSS database scheme from a path.
[[nodiscard]] QString stripNssScheme(const QString& path);
/// Returns the "sql:"-prefixed canonical filesystem path for a configured
/// database, accepting URL-form values such as "file:/tmp/nssdb".
[[nodiscard]] QString canonicalNssDatabasePath(const QString& path);
/// Chooses the first existing database path in configuration priority order.
[[nodiscard]] QString chooseNssDbPath(const QString& envDb,
                                      bool envExists,
                                      const QString& userPkiDb,
                                      bool userExists,
                                      const QString& sysPkiDb,
                                      bool sysExists);

} // namespace Mu::Plugin::Crypto::Internal

#endif // MU_PLUGIN_CRYPTO_NSS_INTERNAL_HPP
