// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/document_factory.hpp"

#include "engine/epub/document.hpp"
#include "engine/kindle/document.hpp"
#include "engine/mobi/document.hpp"
#include "engine/pdf/document.hpp"

namespace Mu::Worker::Engine {

std::unique_ptr<DocumentBase> createDocument(Model::DocumentType type, std::size_t storeSize)
{
    switch (type) {
    case Model::DocumentType::Pdf:
        return std::make_unique<PdfDocument>(storeSize);
    case Model::DocumentType::Epub:
        return std::make_unique<EpubDocument>(storeSize);
    case Model::DocumentType::Mobi:
        return std::make_unique<MobiDocument>(storeSize);
    case Model::DocumentType::Kindle:
        return std::make_unique<KindleDocument>(storeSize);
    default:
        return nullptr;
    }
}

} // namespace Mu::Worker::Engine
