// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "manager_dialog.hpp"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

#include "dialog_utils.hpp"
#include "plugin/crypto/certificate_database.hpp"
#include "self_signed_dialog.hpp"

namespace Mu::Generator {

namespace {

QString formatDate(const Model::Timestamp& timestamp)
{
    // NSS can omit validity metadata; avoid presenting an epoch date as real.
    if (!timestamp.valid)
        return QObject::tr("Unknown");
    return QDateTime::fromMSecsSinceEpoch(timestamp.unixMilliseconds).toString(Qt::ISODate);
}

QString certificateName(const Model::Certificate& certificate)
{
    // Nicknames are the user-facing NSS identity; fall back to the subject
    // when a certificate was imported without one.
    return certificate.nickname.empty() ? QString::fromStdString(certificate.subjectCommonName)
                                        : QString::fromStdString(certificate.nickname);
}

} // namespace

CertificateManagerDialog::CertificateManagerDialog(QString databasePath, QWidget* parent)
    : QDialog(parent)
    , m_table(new QTableWidget(this))
    , m_addButton(new QPushButton(tr("Add Certificate"), this))
    , m_deleteButton(new QPushButton(tr("Delete Selected"), this))
    , m_closeButton(new QPushButton(tr("Close"), this))
    , m_databasePath(std::move(databasePath))
{
    // Step 1: Build a read-only table; all edits are explicit button actions.
    setWindowTitle(CertificateManager::dialogTitle(tr("Manage NSS Certificates"), m_databasePath));
    resize(760, 420);

    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        { tr("Nickname"), tr("Subject"), tr("Issuer"), tr("Valid From"), tr("Valid Until") });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_addButton);
    buttons->addWidget(m_deleteButton);
    buttons->addStretch();
    buttons->addWidget(m_closeButton);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Signing certificates in this NSS database:"), this));
    layout->addWidget(m_table);
    layout->addLayout(buttons);

    m_deleteButton->setEnabled(false);
    m_closeButton->setDefault(true);
    // Step 2: Keep the delete action tied to the current row and route all
    // mutations through slots so the table can be refreshed from NSS.
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        m_deleteButton->setEnabled(m_table->currentRow() >= 0);
    });
    connect(m_addButton, &QPushButton::clicked, this, &CertificateManagerDialog::addCertificate);
    connect(m_deleteButton, &QPushButton::clicked, this, &CertificateManagerDialog::deleteSelectedCertificate);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
    // Step 3: Show the current database contents as soon as the dialog opens.
    refreshCertificates();
}

void CertificateManagerDialog::refreshCertificates()
{
    // Query NSS on every refresh so imports/deletions made by this dialog are
    // reflected without maintaining a second in-memory certificate list.
    QString error;
    const auto certificates = Plugin::Crypto::CertificateDatabase::listCertificates(m_databasePath, &error);
    if (!error.isEmpty()) {
        m_table->clearContents();
        m_table->setRowCount(0);
        m_deleteButton->setEnabled(false);
        showWarning(tr("Certificate Database"), error);
        return;
    }
    m_table->setRowCount(static_cast<int>(certificates.size()));
    for (int row = 0; row < static_cast<int>(certificates.size()); ++row) {
        const auto& certificate = certificates.at(row);
        const QStringList values { certificateName(certificate),
                                   QString::fromStdString(certificate.subjectCommonName),
                                   QString::fromStdString(certificate.issuerCommonName),
                                   formatDate(certificate.validityStart),
                                   formatDate(certificate.validityEnd) };
        for (int column = 0; column < values.size(); ++column)
            m_table->setItem(row, column, new QTableWidgetItem(values.at(column)));
        m_table->item(row, 0)->setData(Qt::UserRole, QString::fromStdString(certificate.nickname));
    }
    m_deleteButton->setEnabled(false);
}

void CertificateManagerDialog::showWarning(const QString& title, const QString& message)
{
    // Include the database path because multiple NSS stores may be configured.
    QMessageBox::warning(this, CertificateManager::dialogTitle(title, m_databasePath), message);
}

void CertificateManagerDialog::addCertificate()
{
    // Keep the menu at the add button so all creation paths share one entry
    // point while retaining their distinct input formats.
    QMenu menu(this);
    QAction* fileAction = menu.addAction(tr("Import from File..."));
    QAction* pasteAction = menu.addAction(tr("Paste PEM Certificate..."));
    QAction* selfSignedAction = menu.addAction(tr("Create Self-Signed Certificate..."));
    QAction* selected = menu.exec(m_addButton->mapToGlobal(QPoint(0, m_addButton->height())));
    if (!selected)
        return;

    if (selected == selfSignedAction) {
        createSelfSignedCertificate();
        return;
    }

    if (selected == fileAction)
        importCertificateFile();
    else if (selected == pasteAction)
        pasteCertificate();
}

void CertificateManagerDialog::createSelfSignedCertificate()
{
    // The child dialog only collects values; NSS key/certificate creation is
    // performed after acceptance and errors remain in this parent dialog.
    SelfSignedCertificateDialog dialog(m_databasePath, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(
            m_databasePath, dialog.certificateOptions(), &error)) {
        showWarning(tr("Create Certificate"), error);
        return;
    }
    refreshCertificates();
}

void CertificateManagerDialog::importCertificateFile()
{
    // Read the complete file before handing bytes to the crypto adapter; the
    // selected extension determines whether a PKCS#12 password is required.
    const QString path =
        QFileDialog::getOpenFileName(this,
                                     CertificateManager::dialogTitle(tr("Import Certificate"), m_databasePath),
                                     { },
                                     tr("PKCS#12 Bundles (*.p12 *.pfx);;PEM Certificates (*.pem);;All Files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showWarning(tr("Import Certificate"), file.errorString());
        return;
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        showWarning(tr("Import Certificate"), file.errorString());
        return;
    }
    if (path.endsWith(QStringLiteral(".p12"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".pfx"), Qt::CaseInsensitive)) {
        bool accepted = false;
        const QString password =
            QInputDialog::getText(this,
                                  CertificateManager::dialogTitle(tr("PKCS#12 Password"), m_databasePath),
                                  tr("Bundle password:"),
                                  QLineEdit::Password,
                                  { },
                                  &accepted);
        if (!accepted)
            return;
        QString error;
        if (!Plugin::Crypto::CertificateDatabase::importPkcs12(m_databasePath, data, password, &error)) {
            showWarning(tr("Import Certificate"), error);
            return;
        }
        refreshCertificates();
        return;
    }

    editCertificateData(data);
}

void CertificateManagerDialog::pasteCertificate()
{
    // An empty initial value distinguishes paste from file-based editing.
    editCertificateData({ });
}

void CertificateManagerDialog::editCertificateData(const QByteArray& initialData)
{
    // Keep the editor local to this operation; import happens only after the
    // user accepts the modal dialog.
    QDialog pasteDialog(this);
    pasteDialog.setWindowTitle(CertificateManager::dialogTitle(tr("Paste PEM Certificate"), m_databasePath));
    auto* layout = new QVBoxLayout(&pasteDialog);
    auto* editor = new QTextEdit(&pasteDialog);
    editor->setPlaceholderText(tr("Paste a PEM certificate here..."));
    if (!initialData.isEmpty())
        editor->setPlainText(QString::fromUtf8(initialData));
    layout->addWidget(editor);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &pasteDialog);
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &pasteDialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &pasteDialog, &QDialog::reject);
    if (pasteDialog.exec() != QDialog::Accepted)
        return;
    importCertificateData(editor->toPlainText().toUtf8());
}

void CertificateManagerDialog::importCertificateData(const QByteArray& data)
{
    // Reject empty input before prompting for an NSS nickname.
    if (data.isEmpty()) {
        showWarning(tr("Import Certificate"), tr("The certificate data is empty"));
        return;
    }

    bool accepted = false;
    const QString nickname =
        QInputDialog::getText(this,
                              CertificateManager::dialogTitle(tr("Certificate Nickname"), m_databasePath),
                              tr("Nickname:"),
                              QLineEdit::Normal,
                              { },
                              &accepted);
    if (!accepted || nickname.trimmed().isEmpty())
        return;

    QString error;
    // CertificateDatabase performs parsing and private-key checks; refresh only
    // after it reports a successful import.
    if (!Plugin::Crypto::CertificateDatabase::importCertificate(m_databasePath, data, nickname, &error)) {
        showWarning(tr("Import Certificate"), error);
        return;
    }
    refreshCertificates();
}

void CertificateManagerDialog::deleteSelectedCertificate()
{
    // The nickname is stored in UserRole rather than reconstructed from the
    // display text, which may fall back to the subject common name.
    const int row = m_table->currentRow();
    if (row < 0)
        return;
    const QString nickname = m_table->item(row, 0)->data(Qt::UserRole).toString();
    if (QMessageBox::question(this,
                              CertificateManager::dialogTitle(tr("Delete Certificate"), m_databasePath),
                              tr("Delete certificate \"%1\" from the NSS database?").arg(nickname))
        != QMessageBox::Yes)
        return;

    QString error;
    if (!Plugin::Crypto::CertificateDatabase::deleteCertificate(m_databasePath, nickname, &error)) {
        showWarning(tr("Delete Certificate"), error);
        return;
    }
    refreshCertificates();
}

} // namespace Mu::Generator
