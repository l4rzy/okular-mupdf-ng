// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_GENERATOR_CONFIG_SIGNATURE_EMBLEM_TYPE_HPP
#define MU_GENERATOR_CONFIG_SIGNATURE_EMBLEM_TYPE_HPP

namespace Mu::Generator::Config {

// Values match the persisted KConfig choices; checked against generated enums.
enum class SignatureEmblem { None = 0, MuPDF = 1, Okular = 2 };

} // namespace Mu::Generator::Config
#endif
