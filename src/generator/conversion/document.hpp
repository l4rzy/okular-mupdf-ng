// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_GENERATOR_CONVERSION_DOCUMENT_HPP
#define MUPDF_GENERATOR_CONVERSION_DOCUMENT_HPP

#include <okular/core/action.h>
#include <okular/core/document.h>
#include <okular/core/fontinfo.h>

#include <QList>

#include <memory>
#include <vector>

#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

Okular::FontInfo fromModel(const Model::Font& font);
std::unique_ptr<Okular::EmbeddedFile> embeddedFile(const Model::EmbeddedFile& file);
QList<Okular::ObjectRect*> objectRects(const std::vector<Model::Link>& links);
std::unique_ptr<Okular::DocumentSynopsis> documentSynopsis(const std::vector<Model::OutlineNode>& nodes);

} // namespace Mu::Generator::Conversion

#endif
