// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_TOOLS_CLI_LAYOUT_CONFIG_HPP
#define MU_TOOLS_CLI_LAYOUT_CONFIG_HPP

#include <QStringList>

#include "shared/model/types.hpp"

namespace Mu::Tools::Cli {

/// Reads Okular's worker settings without KDE, using XDG configuration locations.
/// Uses Qt INI syntax; KDE-specific flags and escaping are unsupported.
Model::DocumentSettings readLayoutConfig();
/// Explicit configuration sources, ordered from lowest to highest precedence.
Model::DocumentSettings readLayoutConfig(const QStringList& files);

} // namespace Mu::Tools::Cli
#endif
