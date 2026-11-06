// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_WORKER_ENGINE_EXPORT_FORMATS_HPP
#define MU_WORKER_ENGINE_EXPORT_FORMATS_HPP
#include "shared/model/export.hpp"

#include <utility>

namespace Mu::Worker::Engine {

using ::Mu::Model::DocumentType;
using ::Mu::Model::OutputFormatInfo;

// Inventory of the writers compiled into the current engine factory, not inferred
// from a suffix/MIME hint. Unknown backends advertise nothing. Revalidated on export.
inline std::vector<OutputFormatInfo> outputFormatsFor(DocumentType type)
{
    using Model::ExportContent;
    using Model::ExportFlag;
    using Model::ExportProfileId;
    using Model::ExportRepresentation;
    constexpr auto pages = std::to_underlying(ExportFlag::PageSelection);
    constexpr auto annotations = std::to_underlying(ExportFlag::IncludeAnnotations);
    constexpr auto flattened = std::to_underlying(ExportFlag::FlattenAnnotations);
    constexpr auto references = std::to_underlying(ExportFlag::IncludeReferences);

    OutputFormatInfo pdf { .id = "pdf", .mimeType = "application/pdf", .extensions = { "pdf" }, .profiles = { } };
    switch (type) {
    case DocumentType::Pdf:
        pdf.profiles = {
            { .id = ExportProfileId::Native, .supportedFlags = annotations, .includesUnsavedChanges = true },
            { .id = ExportProfileId::Print,
              .representation = ExportRepresentation::Vector,
              .supportedFlags = pages,
              .includesUnsavedChanges = true },
            { .id = ExportProfileId::Flattened,
              .representation = ExportRepresentation::Vector,
              .supportedFlags = pages | annotations | flattened,
              .includesUnsavedChanges = true },
        };
        break;
    case DocumentType::Xps:
        pdf.profiles = {
            { .id = ExportProfileId::Print, .representation = ExportRepresentation::Vector, .supportedFlags = pages }
        };
        break;
    case DocumentType::Epub:
    case DocumentType::Mobi:
    case DocumentType::Kindle:
        pdf.profiles = {
            { .id = ExportProfileId::Print, .representation = ExportRepresentation::Vector, .supportedFlags = pages },
            { .id = ExportProfileId::WithReferences,
              .representation = ExportRepresentation::Vector,
              .supportedFlags = pages | references },
            { .id = ExportProfileId::SourceWithReferences,
              .representation = ExportRepresentation::Vector,
              .content = ExportContent::OriginalSource,
              .supportedFlags = pages | references,
              .synchronous = false,
              .asynchronous = true,
              .cancellation = Model::ExportCancellation::DrainAndDiscard },
        };
        break;
    default:
        return { };
    }

    std::vector<OutputFormatInfo> formats { std::move(pdf) };
    switch (type) {
    case DocumentType::Epub:
        formats.push_back(
            { .id = "azw3",
              .mimeType = "application/vnd.amazon.mobi8-ebook",
              .extensions = { "azw3" },
              .profiles = { { .id = ExportProfileId::Converted, .content = ExportContent::OriginalSource } } });
        break;
    case DocumentType::Kindle:
        formats.push_back(
            { .id = "epub",
              .mimeType = "application/epub+zip",
              .extensions = { "epub" },
              .profiles = { { .id = ExportProfileId::Converted, .content = ExportContent::OriginalSource } } });
        break;
    default:
        break;
    }
    return formats;
}

} // namespace Mu::Worker::Engine
#endif
