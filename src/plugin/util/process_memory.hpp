// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_PROCESS_MEMORY_HPP
#define MU_PLUGIN_UTIL_PROCESS_MEMORY_HPP

#include <optional>

#include <QString>

namespace Mu::Plugin::Util {

/// Resident memory of a process in bytes, or nullopt when the process does not
/// exist or its status is unreadable. Implemented via /proc; only Linux is
/// supported and every other platform always returns nullopt.
std::optional<quint64> processResidentBytes(qint64 pid);

} // namespace Mu::Plugin::Util

#endif // MU_PLUGIN_UTIL_PROCESS_MEMORY_HPP
