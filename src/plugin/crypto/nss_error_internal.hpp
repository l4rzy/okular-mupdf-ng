// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_PLUGIN_CRYPTO_NSS_ERROR_INTERNAL_HPP
#define MUPDF_PLUGIN_CRYPTO_NSS_ERROR_INTERNAL_HPP

#include <QString>

#pragma push_macro("slots")
#undef slots
#include <prerror.h>
#pragma pop_macro("slots")

namespace Mu::Plugin::Crypto::Internal {

/// Converts an NSS error code into a user- and log-friendly message.
[[nodiscard]] QString nssErrorMessage(PRErrorCode error);

} // namespace Mu::Plugin::Crypto::Internal

#endif // MUPDF_PLUGIN_CRYPTO_NSS_ERROR_INTERNAL_HPP
