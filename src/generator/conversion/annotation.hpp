// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_GENERATOR_CONVERSION_ANNOTATION_HPP
#define MUPDF_GENERATOR_CONVERSION_ANNOTATION_HPP

#include <okular/core/annotations.h>

#include <memory>
#include <optional>

#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

std::unique_ptr<Okular::Annotation> fromModel(const Model::Annotation& annotation);
std::optional<Model::Annotation> toModel(const Okular::Annotation* annotation);

} // namespace Mu::Generator::Conversion

#endif
