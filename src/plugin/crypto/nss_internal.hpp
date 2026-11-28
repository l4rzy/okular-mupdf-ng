// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CRYPTO_NSS_INTERNAL_HPP
#define MU_PLUGIN_CRYPTO_NSS_INTERNAL_HPP

#include <QString>

namespace Mu::Plugin::Crypto::Internal {

/// Removes a supported NSS database scheme from a path.
[[nodiscard]] QString stripNssScheme(const QString& path);
/// Returns the "sql:"-prefixed canonical filesystem path for a configured
/// database, accepting URL-form values such as "file:/tmp/nssdb".
[[nodiscard]] QString canonicalNssDatabasePath(const QString& path);
/// Chooses the first existing database path in configuration priority order.
[[nodiscard]] QString chooseNssDbPath(const QString& envDb,
                                      bool envExists,
                                      const QString& userPkiDb,
                                      bool userExists,
                                      const QString& sysPkiDb,
                                      bool sysExists);

} // namespace Mu::Plugin::Crypto::Internal

#endif // MU_PLUGIN_CRYPTO_NSS_INTERNAL_HPP
