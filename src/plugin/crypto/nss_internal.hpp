// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CRYPTO_NSS_INTERNAL_HPP
#define MUPDF_PLUGIN_CRYPTO_NSS_INTERNAL_HPP

#pragma push_macro("slots")
#undef slots
#include <secoidt.h>
#pragma pop_macro("slots")

#include <mutex>

#include <QString>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Crypto::Internal {

/// Returns the mutex protecting process-global NSS state.
std::mutex& nssMutex();
/// Maps an NSS digest identifier to the transport model's hash enumeration.
[[nodiscard]] Model::HashAlgorithm hashAlgorithmForDigest(SECOidTag digestAlgorithm);
/// Removes a supported NSS database scheme from a path.
[[nodiscard]] QString stripNssScheme(const QString& path);
/// Chooses the first existing database path in configuration priority order.
[[nodiscard]] QString chooseNssDbPath(const QString& envDb,
                                      bool envExists,
                                      const QString& userPkiDb,
                                      bool userExists,
                                      const QString& sysPkiDb,
                                      bool sysExists);

} // namespace Mu::Plugin::Crypto::Internal

#endif // MUPDF_PLUGIN_CRYPTO_NSS_INTERNAL_HPP
