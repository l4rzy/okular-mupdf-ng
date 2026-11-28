// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CRYPTO_NSS_HPP
#define MU_PLUGIN_CRYPTO_NSS_HPP

#include <QList>
#include <QString>
#include <paperbox/nss.hpp>

namespace Mu::Plugin::Crypto {

using NssRuntimeMode = paperbox::nss::RuntimeMode;

[[nodiscard]] NssRuntimeMode initializeNss(const QString& databasePath = { });
[[nodiscard]] NssRuntimeMode activeNssMode();
[[nodiscard]] QString activeNssDatabasePath();
[[nodiscard]] bool isNssDatabaseActive(const QString& databasePath);
[[nodiscard]] QString defaultSystemNssDbPath();
[[nodiscard]] QList<paperbox::model::Certificate> signingCertificates();
[[nodiscard]] bool checkSigningCertificatePassword(const QString& nickname, const QString& password);
[[nodiscard]] QString signingCertificateCommonName(const QString& nickname);

} // namespace Mu::Plugin::Crypto

#endif // MU_PLUGIN_CRYPTO_NSS_HPP
