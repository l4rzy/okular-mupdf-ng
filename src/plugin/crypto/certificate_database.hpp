// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CRYPTO_CERTIFICATE_DATABASE_HPP
#define MU_PLUGIN_CRYPTO_CERTIFICATE_DATABASE_HPP

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QtGlobal>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Crypto::CertificateDatabase {

/// Identifies a certificate within one NSS PKCS#11 slot.
struct CertificateIdentity {
    quint64 moduleId = 0;
    quint64 slotId = 0;
    QByteArray sha256Fingerprint;
};

/// Certificate-manager data: display fields plus the exact deletion target.
struct CertificateRecord {
    Model::Certificate certificate;
    CertificateIdentity identity;
};

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

/// Lists internal-slot certificates with private keys from the selected persistent NSS database.
[[nodiscard]] QList<CertificateRecord> listCertificates(const QString& databasePath, QString* error = nullptr);
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
/// Removes the exact internal-slot certificate and its associated persistent key objects.
bool deleteCertificate(const QString& databasePath, const CertificateIdentity& identity, QString* error = nullptr);

} // namespace Mu::Plugin::Crypto::CertificateDatabase

#endif // MU_PLUGIN_CRYPTO_CERTIFICATE_DATABASE_HPP
