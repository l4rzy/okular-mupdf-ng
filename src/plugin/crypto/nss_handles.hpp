// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CRYPTO_NSS_HANDLES_HPP
#define MU_PLUGIN_CRYPTO_NSS_HANDLES_HPP

#pragma push_macro("slots")
#undef slots
#include <paperbox/nss/database.hpp>
#include <paperbox/nss/handles.hpp>
#pragma pop_macro("slots")

#include <QByteArray>
#include <utility>

namespace Mu::Plugin::Crypto {

/// Owns byte storage that is wiped when the object is destroyed.
class SensitiveBytes {
public:
    explicit SensitiveBytes(QByteArray value)
        : bytes(std::move(value))
    {
    }

    ~SensitiveBytes() { bytes.fill('\0'); }

    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;
    /// Moving transfers the buffer to a new owner; move assignment is forbidden because it could skip wiping.
    SensitiveBytes(SensitiveBytes&&) noexcept = default;
    SensitiveBytes& operator=(SensitiveBytes&&) = delete;

    QByteArray bytes;
};

} // namespace Mu::Plugin::Crypto

#endif // MU_PLUGIN_CRYPTO_NSS_HANDLES_HPP
