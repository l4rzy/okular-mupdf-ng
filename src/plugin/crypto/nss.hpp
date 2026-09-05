// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CRYPTO_NSS_HPP
#define MUPDF_PLUGIN_CRYPTO_NSS_HPP

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QString>

#include <array>
#include <cstdint>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Crypto {

struct CmsResult {
    /// Outcome reported to the signing caller.
    Model::SigningResult result = Model::SigningResult::GenericError;
    /// Human-readable detail for a failed operation.
    QString details;
    /// Detached CMS bytes when signing succeeds.
    QByteArray cms;
};

/// Capability of the process-wide NSS runtime selected during initialization.
enum class NssRuntimeMode {
    Unavailable,
    NoDb,
    ReadOnly,
    ReadWrite,
};

/// Initializes process-wide NSS using read/write, read-only, then NoDB mode.
/// Once initialized, the process cannot switch to a different database.
/// The empty path denotes the defaulted selection and may create the missing
/// default database directory; an explicit path is never auto-created.
/// Returns Unavailable for a conflicting explicit path without changing the
/// runtime reported by activeNssMode().
[[nodiscard]] NssRuntimeMode initializeNss(const QString& databasePath = { });
/// Returns the capability of the active process-wide NSS runtime.
[[nodiscard]] NssRuntimeMode activeNssMode();
/// Returns the active persistent database path without its NSS scheme prefix.
[[nodiscard]] QString activeNssDatabasePath();
/// Reports whether the requested database is the active NSS database.
[[nodiscard]] bool isNssDatabaseActive(const QString& databasePath);
/// Selects the first existing database from the configured system locations.
[[nodiscard]] QString defaultSystemNssDbPath();
/// Lists certificates that NSS can use for signing.
[[nodiscard]] QList<Model::Certificate> signingCertificates();
/// Checks the password for the private key associated with a certificate.
[[nodiscard]] bool checkSigningCertificatePassword(const QString& certNickname, const QString& password);
/// Returns the subject Common Name, falling back to the subject DN or nickname.
[[nodiscard]] QString signingCertificateCommonName(const QString& certNickname);
/// Creates detached CMS bytes for a caller-computed SHA-256 digest.
[[nodiscard]] CmsResult createDetachedCmsFromDigest(const QString& certNickname,
                                                    const QString& password,
                                                    const std::array<std::uint8_t, 32>& digest);

/// Validates untrusted detached PDF signature material and restores the source
/// position when it was readable. When `checkOcsp` is set, OCSP revocation
/// checking is enabled on the certificate database for the duration of the
/// validation (soft-fail: an unreachable responder keeps the chain-derived
/// status).
void validateDetachedPdfSignature(Model::SignatureField& field, QIODevice& source, bool checkOcsp = false);

} // namespace Mu::Plugin::Crypto

#endif // MUPDF_PLUGIN_CRYPTO_NSS_HPP
