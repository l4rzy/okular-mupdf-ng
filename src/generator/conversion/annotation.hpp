// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONVERSION_ANNOTATION_HPP
#define MU_GENERATOR_CONVERSION_ANNOTATION_HPP

#include <okular/core/annotations.h>

#include <memory>
#include <optional>

#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

std::unique_ptr<Okular::Annotation> fromModel(const Model::Annotation& annotation);
std::optional<Model::Annotation> toModel(const Okular::Annotation* annotation);
// Discards all annotations on the page and rebuilds them from clean worker
// state, mirroring initial document load. Unsupported subtypes are skipped.
void rebuildPageAnnotations(Okular::Page* page, const std::vector<Model::Annotation>& clean);

} // namespace Mu::Generator::Conversion

#endif // MU_GENERATOR_CONVERSION_ANNOTATION_HPP
