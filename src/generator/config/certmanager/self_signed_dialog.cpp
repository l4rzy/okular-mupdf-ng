// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "self_signed_dialog.hpp"

#include "dialog_utils.hpp"

#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include <utility>

namespace Mu::Generator {

SelfSignedCertificateDialog::SelfSignedCertificateDialog(QString databasePath, QWidget* parent)
    : QDialog(parent)
    , m_nickname(new QLineEdit(this))
    , m_commonName(new QLineEdit(this))
    , m_organization(new QLineEdit(this))
    , m_organizationalUnit(new QLineEdit(this))
    , m_locality(new QLineEdit(this))
    , m_state(new QLineEdit(this))
    , m_country(new QLineEdit(this))
    , m_validFrom(new QDateTimeEdit(QDateTime::currentDateTime(), this))
    , m_validUntil(new QDateTimeEdit(QDateTime::currentDateTime().addYears(1), this))
    , m_databasePath(std::move(databasePath))
{
    // Step 1: Collect the subject identity fields used to construct the cert.
    setWindowTitle(CertificateManager::dialogTitle(tr("Create Self-Signed Certificate"), m_databasePath));

    auto* subjectGroup = new QGroupBox(tr("Certificate identity"), this);
    auto* subjectForm = new QFormLayout(subjectGroup);
    subjectForm->addRow(tr("Nickname:"), m_nickname);
    subjectForm->addRow(tr("Common Name:"), m_commonName);
    subjectForm->addRow(tr("Organization:"), m_organization);
    subjectForm->addRow(tr("Organizational Unit:"), m_organizationalUnit);
    subjectForm->addRow(tr("Locality:"), m_locality);
    subjectForm->addRow(tr("State or Province:"), m_state);
    subjectForm->addRow(tr("Country (2 letters):"), m_country);
    for (auto* field : { m_nickname, m_commonName, m_organization, m_organizationalUnit, m_locality, m_state })
        field->setMaxLength(256);
    m_nickname->setMaxLength(128);
    m_country->setMaxLength(2);
    // NSS expects an ISO-style two-letter country code when one is supplied.
    m_country->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z]{0,2}")), m_country));

    auto* validityGroup = new QGroupBox(tr("Validity and key"), this);
    auto* validityForm = new QFormLayout(validityGroup);
    m_validFrom->setCalendarPopup(true);
    m_validUntil->setCalendarPopup(true);

    // Step 2: Present validity and key details in a compact, aligned form.
    const auto addValueRow = [validityForm, validityGroup](const QString& labelText, QWidget* value) {
        auto* row = new QWidget(validityGroup);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);

        rowLayout->addWidget(new QLabel(labelText, row));

        auto* line = new QFrame(row);
        line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        line->setStyleSheet(QStringLiteral("border: none; border-top: 1px solid #444444;"));
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Plain);
        rowLayout->addWidget(line);
        rowLayout->addWidget(value);

        validityForm->addRow(row);
    };

    addValueRow(tr("Valid from:"), m_validFrom);
    addValueRow(tr("Valid until:"), m_validUntil);
    addValueRow(tr("Signing key:"), new QLabel(tr("RSA 2048"), validityGroup));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Step 3: Accepting closes the dialog; the parent performs NSS generation.
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(subjectGroup);
    layout->addWidget(validityGroup);
    layout->addWidget(buttons);
    m_nickname->setFocus();
    resize(520, 460);
}

Plugin::Crypto::CertificateDatabase::SelfSignedCertificateOptions
SelfSignedCertificateDialog::certificateOptions() const
{
    // Preserve field order required by SelfSignedCertificateOptions; no NSS
    // operation is performed while the dialog is merely being inspected.
    return { m_nickname->text(),      m_commonName->text(), m_organization->text(), m_organizationalUnit->text(),
             m_locality->text(),      m_state->text(),      m_country->text(),      m_validFrom->dateTime(),
             m_validUntil->dateTime() };
}

} // namespace Mu::Generator
