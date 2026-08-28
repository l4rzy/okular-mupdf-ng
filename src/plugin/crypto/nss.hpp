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

/// Initializes process-wide NSS, preferring the requested persistent database.
/// NoDB fallback is opt-in because it cannot expose the user's certificate store.
bool ensureNssInitialized(const QString& databasePath = { }, bool allowNoDbFallback = false);
/// Returns the active persistent database path without its NSS scheme prefix.
[[nodiscard]] QString activeNssDatabasePath();
/// Reports whether the requested database is the active NSS database.
[[nodiscard]] bool isNssDatabaseActive(const QString& databasePath);
/// Reports whether NSS is initialized with a persistent certificate database.
[[nodiscard]] bool hasPersistentNssDatabase();
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
/// position when it was readable.
void validateDetachedPdfSignature(Model::SignatureField& field, QIODevice& source);

} // namespace Mu::Plugin::Crypto

#endif // MUPDF_PLUGIN_CRYPTO_NSS_HPP
