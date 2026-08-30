// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_CERTIFICATE_MANAGER_DIALOG_HPP
#define MUPDF_CERTIFICATE_MANAGER_DIALOG_HPP

#include <QByteArray>
#include <QDialog>
#include <QList>

#include "plugin/crypto/certificate_database.hpp"

class QTableWidget;
class QPushButton;

namespace Mu::Generator {

/// Modal editor for certificates stored in the active NSS database.
///
/// Certificate records are read and modified through CertificateDatabase, so
/// NSS remains the source of truth.
class CertificateManagerDialog final : public QDialog {
    Q_OBJECT

public:
    explicit CertificateManagerDialog(QString databasePath, QWidget* parent = nullptr);

private:
    // Rebuilds the table from NSS after every successful database mutation.
    void refreshCertificates();
    // Presents the supported certificate creation/import operations.
    void addCertificate();
    // Reads a PEM or PKCS#12 file selected by the user.
    void importCertificateFile();
    // Opens the editor used for pasted certificate data.
    void pasteCertificate();
    // Shows the editor and forwards accepted text to the database adapter.
    void editCertificateData(const QByteArray& initialData);
    // Validates the pasted bytes and collects the NSS nickname before import.
    void importCertificateData(const QByteArray& certificateData);
    // Collects self-signed options and stores the resulting certificate.
    void createSelfSignedCertificate();
    // Deletes the certificate identified by the selected row's stored record.
    void deleteSelectedCertificate();
    // Adds the selected NSS database to the warning title for disambiguation.
    void showWarning(const QString& title, const QString& message);

    QTableWidget* m_table;
    QPushButton* m_addButton;
    QPushButton* m_deleteButton;
    QPushButton* m_closeButton;
    QList<Plugin::Crypto::CertificateDatabase::CertificateRecord> m_certificates;
    // Empty means NSS's default database; otherwise this is the selected DB.
    QString m_databasePath;
};

} // namespace Mu::Generator

#endif
