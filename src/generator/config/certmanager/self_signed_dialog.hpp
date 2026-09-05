// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_CERTMANAGER_SELF_SIGNED_DIALOG_HPP
#define MU_GENERATOR_CONFIG_CERTMANAGER_SELF_SIGNED_DIALOG_HPP

#include <QDialog>

#include "plugin/crypto/certificate_database.hpp"

class QDateTimeEdit;
class QLineEdit;

namespace Mu::Generator {

/// Collects the subject and validity fields for a new RSA self-signed cert.
///
/// This dialog only gathers input. Certificate generation and NSS persistence
/// are performed by CertificateDatabase after the dialog is accepted.
class SelfSignedCertificateDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SelfSignedCertificateDialog(QString databasePath, QWidget* parent = nullptr);

    // Converts the current widget values into the database API's value object.
    Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions certificateOptions() const;

private:
    QLineEdit* m_nickname;
    QLineEdit* m_commonName;
    QLineEdit* m_organization;
    QLineEdit* m_organizationalUnit;
    QLineEdit* m_locality;
    QLineEdit* m_state;
    QLineEdit* m_country;
    QDateTimeEdit* m_validFrom;
    QDateTimeEdit* m_validUntil;
    // Used in the title so operations identify which NSS database is edited.
    QString m_databasePath;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_CONFIG_CERTMANAGER_SELF_SIGNED_DIALOG_HPP
