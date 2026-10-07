// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_GENERATOR_CONFIG_OKULAR_EMBLEM_HPP
#define MU_GENERATOR_CONFIG_OKULAR_EMBLEM_HPP

#include <QImage>
#include <cstdint>
#include <vector>

namespace Mu::Generator::Config {

/// Render the detailed QPainter artwork as a faint watermark for both appearances.
[[nodiscard]] QImage renderOkularEmblem();
[[nodiscard]] std::vector<std::uint8_t> encodeOkularEmblem();

} // namespace Mu::Generator::Config
#endif
