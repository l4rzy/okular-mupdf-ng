// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CRYPTO_CERTIFICATE_DATABASE_HPP
#define MUPDF_PLUGIN_CRYPTO_CERTIFICATE_DATABASE_HPP

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Crypto::CertificateDatabase {

struct SelfSignedCertificateOptions {
    /// Persistent nickname used to identify the certificate in NSS.
    QString nickname;
    /// Common Name placed in the certificate subject.
    QString commonName;
    /// Optional organization placed in the certificate subject.
    QString organization;
    /// Optional organizational unit placed in the certificate subject.
    QString organizationalUnit;
    /// Optional locality placed in the certificate subject.
    QString locality;
    /// Optional state or province placed in the certificate subject.
    QString state;
    /// Optional two-letter country code placed in the certificate subject.
    QString country;
    /// Start of the certificate validity interval.
    QDateTime validFrom;
    /// End of the certificate validity interval.
    QDateTime validUntil;
};

/// Lists certificates with private keys from the selected persistent NSS database.
[[nodiscard]] QList<Model::Certificate> listCertificates(const QString& databasePath, QString* error = nullptr);
/// Imports a certificate and requires a matching private key to be present.
bool importCertificate(const QString& databasePath,
                       const QByteArray& data,
                       const QString& nickname,
                       QString* error = nullptr);
/// Imports a password-protected PKCS#12 bundle into the selected NSS database.
bool importPkcs12(const QString& databasePath,
                  const QByteArray& data,
                  const QString& password,
                  QString* error = nullptr);
/// Generates and stores a persistent self-signed RSA signing certificate.
bool createSelfSignedCertificate(const QString& databasePath,
                                 const SelfSignedCertificateOptions& options,
                                 QString* error = nullptr);
/// Removes a certificate and its associated persistent key objects.
bool deleteCertificate(const QString& databasePath, const QString& nickname, QString* error = nullptr);

} // namespace Mu::Plugin::Crypto::CertificateDatabase

#endif // MUPDF_PLUGIN_CRYPTO_CERTIFICATE_DATABASE_HPP
