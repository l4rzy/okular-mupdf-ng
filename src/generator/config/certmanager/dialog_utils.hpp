// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_CERTMANAGER_DIALOG_UTILS_HPP
#define MU_GENERATOR_CONFIG_CERTMANAGER_DIALOG_UTILS_HPP

#include <QString>

#include "shared/model/types.hpp"

namespace Mu::Generator::CertificateManager {

// Keep long NSS paths readable in titles while retaining the default marker.
inline QString displayDatabasePath(const QString& databasePath)
{
    if (databasePath.isEmpty())
        return QStringLiteral("Default NSS database");
    if (databasePath.size() <= 30)
        return databasePath;
    return databasePath.left(27) + QStringLiteral("...");
}

// Use the same database context in every certificate-manager dialog title.
inline QString dialogTitle(const QString& title, const QString& databasePath)
{
    return title + QStringLiteral(" — ") + displayDatabasePath(databasePath);
}

// Human-readable public-key algorithm for the certificate table. An unknown
// type yields an empty string so callers can localize the fallback.
inline QString formatKeyAlgorithm(const Model::Certificate& certificate)
{
    QString name;
    switch (static_cast<Model::PublicKeyAlgorithm>(certificate.publicKeyType)) {
    case Model::PublicKeyAlgorithm::Rsa:
        name = QStringLiteral("RSA");
        break;
    case Model::PublicKeyAlgorithm::Dsa:
        name = QStringLiteral("DSA");
        break;
    case Model::PublicKeyAlgorithm::Ec:
        name = QStringLiteral("EC");
        break;
    case Model::PublicKeyAlgorithm::Unknown:
        return { };
    default:
        // The model field is an unconstrained int32 that can also arrive over
        // IPC, so out-of-range values must not fall off the end.
        return { };
    }
    if (certificate.publicKeyStrength > 0)
        return name + QLatin1Char(' ') + QString::number(certificate.publicKeyStrength);
    return name;
}

} // namespace Mu::Generator::CertificateManager

#endif // MU_GENERATOR_CONFIG_CERTMANAGER_DIALOG_UTILS_HPP
