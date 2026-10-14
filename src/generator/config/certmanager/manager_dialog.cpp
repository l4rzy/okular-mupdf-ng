// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "manager_dialog.hpp"

#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <KLocalizedString>

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
        return i18n("Unknown");
    return QDateTime::fromMSecsSinceEpoch(timestamp.unixMilliseconds).toString(Qt::ISODate);
}

QString certificateName(const Model::Certificate& certificate)
{
    // Nicknames are user-facing labels; fall back to the subject when absent.
    return certificate.nickname.empty() ? QString::fromStdString(certificate.subjectCommonName)
                                        : QString::fromStdString(certificate.nickname);
}

} // namespace

CertificateManagerDialog::CertificateManagerDialog(QString databasePath, QWidget* parent)
    : QDialog(parent)
    , m_tabs(new QTabWidget(this))
    , m_table(new QTableWidget(this))
    , m_addButton(new QPushButton(i18n("Add Certificate"), this))
    , m_deleteButton(new QPushButton(i18n("Delete Selected"), this))
    , m_closeButton(new QPushButton(i18n("Close"), this))
    , m_databasePath(std::move(databasePath))
{
    // Step 1: Build a read-only table; all edits are explicit button actions.
    setWindowTitle(CertificateManager::dialogTitle(i18n("Manage NSS Certificates"), m_databasePath));
    resize(760, 420);

    auto* peopleTable = new QTableWidget(this);
    const QList<QTableWidget*> tables { m_table, peopleTable };
    const QStringList tabLabels { i18n("My Certificates"), i18n("People") };
    const QStringList descriptions { i18n("Certificates with private keys used to sign documents."),
                                     i18n("Individual certificates trusted for PDF signature verification. Trust also "
                                          "applies to S/MIME in this NSS database.") };
    for (int index = 0; index < tables.size(); ++index) {
        auto* table = tables.at(index);
        table->setColumnCount(7);
        table->setHorizontalHeaderLabels({ i18n("Nickname"),
                                           i18n("Subject"),
                                           i18n("Issuer"),
                                           i18n("Algorithm"),
                                           i18n("Valid From"),
                                           i18n("Valid Until"),
                                           i18n("SHA-256 Fingerprint") });
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        auto* page = new QWidget(m_tabs);
        auto* pageLayout = new QVBoxLayout(page);
        auto* description = new QLabel(descriptions.at(index), page);
        description->setWordWrap(true);
        pageLayout->addWidget(description);
        pageLayout->addWidget(table);
        m_tabs->addTab(page, tabLabels.at(index));
        connect(table, &QTableWidget::itemSelectionChanged, this, [this] {
            m_deleteButton->setEnabled(m_table->currentRow() >= 0);
        });
    }

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_addButton);
    buttons->addWidget(m_deleteButton);
    buttons->addStretch();
    buttons->addWidget(m_closeButton);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_tabs);
    layout->addLayout(buttons);

    m_deleteButton->setEnabled(false);
    m_closeButton->setDefault(true);
    // Step 2: Keep the delete action tied to the current row and route all
    // mutations through slots so the table can be refreshed from NSS.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        m_table = m_tabs->currentWidget()->findChild<QTableWidget*>();
        m_deleteButton->setText(showsPeople() ? i18n("Remove Trust") : i18n("Delete Selected"));
        refreshCertificates();
    });
    connect(m_addButton, &QPushButton::clicked, this, &CertificateManagerDialog::addCertificate);
    connect(m_deleteButton, &QPushButton::clicked, this, &CertificateManagerDialog::deleteSelectedCertificate);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
    // Step 3: Show the current database contents as soon as the dialog opens.
    refreshCertificates();
}

bool CertificateManagerDialog::showsPeople() const
{
    return m_tabs->currentIndex() == 1;
}

void CertificateManagerDialog::refreshCertificates()
{
    // Query NSS on every refresh so imports/deletions made by this dialog are
    // reflected in both the table and its exact deletion records.
    QString error;
    m_certificates = showsPeople() ? Plugin::Crypto::CertificateDatabase::listTrustedSigners(m_databasePath, &error)
                                   : Plugin::Crypto::CertificateDatabase::listCertificates(m_databasePath, &error);
    if (!error.isEmpty()) {
        m_certificates.clear();
        m_table->clearContents();
        m_table->setRowCount(0);
        m_deleteButton->setEnabled(false);
        showWarning(i18n("Certificate Database"), error);
        return;
    }
    m_table->setRowCount(0);
    m_table->setRowCount(static_cast<int>(m_certificates.size()));
    for (int row = 0; row < static_cast<int>(m_certificates.size()); ++row) {
        const auto& certificate = m_certificates.at(row).certificate;
        const QString algorithm = CertificateManager::formatKeyAlgorithm(certificate);
        const QStringList values { certificateName(certificate),
                                   QString::fromStdString(certificate.subjectCommonName),
                                   QString::fromStdString(certificate.issuerCommonName),
                                   algorithm.isEmpty() ? i18n("Unknown") : algorithm,
                                   formatDate(certificate.validityStart),
                                   formatDate(certificate.validityEnd),
                                   QString::fromLatin1(m_certificates.at(row).identity.sha256Fingerprint.toHex(':')) };
        for (int column = 0; column < values.size(); ++column)
            m_table->setItem(row, column, new QTableWidgetItem(values.at(column)));
        m_table->item(row, 0)->setData(Qt::UserRole, row);
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
    QAction* fileAction = menu.addAction(i18n("Import from File..."));
    QAction* pasteAction = menu.addAction(i18n("Paste PEM Certificate..."));
    QAction* selfSignedAction = showsPeople() ? nullptr : menu.addAction(i18n("Create Self-Signed Certificate..."));
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
    // RSA key generation can take seconds. Keep the GUI responsive while the
    // modal progress dialog prevents concurrent certificate-manager edits.
    QProgressDialog progress(i18n("Creating certificate..."), QString { }, 0, 0, this);
    progress.setWindowTitle(i18n("Create Certificate"));
    progress.setCancelButton(nullptr);
    progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
    using CreationResult = std::pair<bool, QString>;
    QFutureWatcher<CreationResult> watcher;
    connect(&watcher, &QFutureWatcher<CreationResult>::finished, &progress, &QDialog::accept);
    watcher.setFuture(QtConcurrent::run([path = m_databasePath, options = dialog.certificateOptions()] {
        QString error;
        const bool created = Plugin::Crypto::CertificateDatabase::createSelfSignedCertificate(path, options, &error);
        return CreationResult { created, error };
    }));
    progress.exec();
    // Even if application shutdown ends the modal loop, the task must finish
    // before the generator (and its NSS adapter code) can be unloaded.
    watcher.waitForFinished();
    const auto [created, error] = watcher.result();
    if (!created) {
        showWarning(i18n("Create Certificate"), error);
        return;
    }
    refreshCertificates();
}

void CertificateManagerDialog::importCertificateFile()
{
    // Read the complete file before handing bytes to the crypto adapter; the
    // selected extension determines whether a PKCS#12 password is required.
    const QString path = QFileDialog::getOpenFileName(
        this,
        CertificateManager::dialogTitle(i18n("Import Certificate"), m_databasePath),
        { },
        showsPeople() ? i18n("Certificates (*.pem *.der *.crt *.cer);;All Files (*)")
                      : i18n("PKCS#12 Bundles (*.p12 *.pfx);;PEM Certificates (*.pem);;All Files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showWarning(i18n("Import Certificate"), file.errorString());
        return;
    }
    if (showsPeople() && file.size() > Plugin::Crypto::CertificateDatabase::MaxSignerCertificateBytes) {
        showWarning(i18n("Import Certificate"), i18n("The certificate file exceeds the 1 MiB limit."));
        return;
    }
    const QByteArray certificateData =
        showsPeople() ? file.read(Plugin::Crypto::CertificateDatabase::MaxSignerCertificateBytes + 1) : file.readAll();
    if (file.error() != QFileDevice::NoError) {
        showWarning(i18n("Import Certificate"), file.errorString());
        return;
    }
    if (showsPeople()) {
        importCertificateData(certificateData);
        return;
    }
    if (path.endsWith(QStringLiteral(".p12"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".pfx"), Qt::CaseInsensitive)) {
        bool accepted = false;
        const QString password =
            QInputDialog::getText(this,
                                  CertificateManager::dialogTitle(i18n("PKCS#12 Password"), m_databasePath),
                                  i18n("Bundle password:"),
                                  QLineEdit::Password,
                                  { },
                                  &accepted);
        if (!accepted)
            return;
        QString error;
        if (!Plugin::Crypto::CertificateDatabase::importPkcs12(m_databasePath, certificateData, password, &error)) {
            showWarning(i18n("Import Certificate"), error);
            return;
        }
        refreshCertificates();
        return;
    }

    editCertificateData(certificateData);
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
    pasteDialog.setWindowTitle(CertificateManager::dialogTitle(i18n("Paste PEM Certificate"), m_databasePath));
    auto* layout = new QVBoxLayout(&pasteDialog);
    auto* editor = new QTextEdit(&pasteDialog);
    editor->setPlaceholderText(i18n("Paste a PEM certificate here..."));
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

void CertificateManagerDialog::importCertificateData(const QByteArray& certificateData)
{
    // Reject empty input before prompting for an NSS nickname.
    if (certificateData.isEmpty()) {
        showWarning(i18n("Import Certificate"), i18n("The certificate data is empty"));
        return;
    }

    if (showsPeople()) {
        QString error;
        const auto certificate =
            Plugin::Crypto::CertificateDatabase::inspectSignerCertificate(m_databasePath, certificateData, &error);
        if (!certificate) {
            showWarning(i18n("Import Certificate"), error);
            return;
        }
        const QByteArray der(reinterpret_cast<const char*>(certificate->der.data()),
                             static_cast<qsizetype>(certificate->der.size()));
        const QString fingerprint =
            QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex(':'));
        QMessageBox confirmation(QMessageBox::Question,
                                 CertificateManager::dialogTitle(i18n("Trust PDF Signer"), m_databasePath),
                                 i18n("Trust this certificate for PDF signature verification?"),
                                 QMessageBox::Yes | QMessageBox::Cancel,
                                 this);
        confirmation.setTextFormat(Qt::PlainText);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.setInformativeText(
            i18n("Subject: %1\nIssuer: %2\nValid from: %3\nValid until: %4\nSHA-256: %5\n\n"
                 "This also grants S/MIME trust in this NSS database. Reopen documents to refresh signature results.",
                 QString::fromStdString(certificate->subjectDistinguishedName),
                 QString::fromStdString(certificate->issuerDistinguishedName),
                 formatDate(certificate->validityStart),
                 formatDate(certificate->validityEnd),
                 fingerprint));
        if (confirmation.exec() != QMessageBox::Yes)
            return;
    }

    bool accepted = false;
    const QString nickname =
        QInputDialog::getText(this,
                              CertificateManager::dialogTitle(i18n("Certificate Nickname"), m_databasePath),
                              i18n("Nickname:"),
                              QLineEdit::Normal,
                              { },
                              &accepted);
    if (!accepted || nickname.trimmed().isEmpty())
        return;

    QString error;
    // CertificateDatabase performs parsing and private-key checks; refresh only
    // after it reports a successful import.
    const bool imported = showsPeople()
        ? Plugin::Crypto::CertificateDatabase::importTrustedSigner(m_databasePath, certificateData, nickname, &error)
        : Plugin::Crypto::CertificateDatabase::importCertificate(m_databasePath, certificateData, nickname, &error);
    if (!imported) {
        showWarning(i18n("Import Certificate"), error);
        return;
    }
    refreshCertificates();
}

void CertificateManagerDialog::deleteSelectedCertificate()
{
    const int row = m_table->currentRow();
    if (row < 0)
        return;
    const QTableWidgetItem* item = m_table->item(row, 0);
    if (!item)
        return;
    const int certificateIndex = item->data(Qt::UserRole).toInt();
    if (certificateIndex < 0 || certificateIndex >= m_certificates.size())
        return;
    const auto& record = m_certificates.at(certificateIndex);
    const QString name = certificateName(record.certificate);
    if (QMessageBox::question(
            this,
            CertificateManager::dialogTitle(showsPeople() ? i18n("Remove Signer Trust") : i18n("Delete Certificate"),
                                            m_databasePath),
            showsPeople() ? i18n("Remove trust for %1? The certificate and any private key will be kept.", name)
                          : i18n("Delete certificate \"%1\" from the NSS database?", name))
        != QMessageBox::Yes)
        return;

    QString error;
    const bool removed = showsPeople()
        ? Plugin::Crypto::CertificateDatabase::removeSignerTrust(m_databasePath, record.identity, &error)
        : Plugin::Crypto::CertificateDatabase::deleteCertificate(m_databasePath, record.identity, &error);
    if (!removed) {
        showWarning(i18n("Delete Certificate"), error);
        return;
    }
    refreshCertificates();
}

} // namespace Mu::Generator
