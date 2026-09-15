// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONVERSION_SIGNING_HPP
#define MU_GENERATOR_CONVERSION_SIGNING_HPP

#include <okular/core/document.h>

#include "generator/config/settings.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

/// Builds the shared appearance payload for a signing request. Captures the
/// signing instant once so the worker's /M and the appearance text always
/// describe the same timestamp. The simple profile renders only name, reason,
/// and time; location is cleared because the worker renders it unconditionally.
[[nodiscard]] Model::SignatureAppearance toModelSignatureAppearance(const Okular::NewSignatureData& data,
                                                                    const Config::SignatureAppearanceOptions& options);

} // namespace Mu::Generator::Conversion

#endif // MU_GENERATOR_CONVERSION_SIGNING_HPP
