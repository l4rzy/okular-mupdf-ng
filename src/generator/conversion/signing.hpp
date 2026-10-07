// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONVERSION_SIGNING_HPP
#define MU_GENERATOR_CONVERSION_SIGNING_HPP

#include <okular/core/document.h>

#include "generator/config/settings.hpp"
#include "shared/model/types.hpp"
#include <optional>

namespace Mu::Generator::Conversion {

/// Builds the shared appearance payload for a signing request. Captures the
/// signing instant once so the worker's /M and the appearance text always
/// describe the same timestamp. The simple profile text contains only name,
/// reason, and time; location is retained as signature metadata. The emblem
/// is selected independently of the profile. Returns empty if emblem encoding fails.
[[nodiscard]] std::optional<Model::SignatureAppearance>
toModelSignatureAppearance(const Okular::NewSignatureData& data, const Config::SignatureAppearanceOptions& options);

} // namespace Mu::Generator::Conversion

#endif // MU_GENERATOR_CONVERSION_SIGNING_HPP
