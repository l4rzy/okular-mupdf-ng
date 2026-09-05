// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_CERTMANAGER_DIALOG_UTILS_HPP
#define MU_GENERATOR_CONFIG_CERTMANAGER_DIALOG_UTILS_HPP

#include <QString>

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

} // namespace Mu::Generator::CertificateManager

#endif // MU_GENERATOR_CONFIG_CERTMANAGER_DIALOG_UTILS_HPP
