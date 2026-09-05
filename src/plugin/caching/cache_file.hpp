// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CACHING_CACHE_FILE_HPP
#define MU_PLUGIN_CACHING_CACHE_FILE_HPP

#include <optional>

#include <QByteArray>
#include <QString>

namespace Mu::Plugin::Caching {

void setRootForTesting(const QString& root);
void clearRootForTesting();

[[nodiscard]] QString directory(const QString& name);
[[nodiscard]] std::optional<QByteArray> readBounded(const QString& path, qint64 maxBytes);
[[nodiscard]] bool writeAtomically(const QString& path, const QByteArray& data);

} // namespace Mu::Plugin::Caching

#endif // MU_PLUGIN_CACHING_CACHE_FILE_HPP
