// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_SHARED_PROTOCOL_LIMITS_HPP
#define MUPDF_SHARED_PROTOCOL_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <limits>

namespace Mu::Limit {

// --- Transport & Control Channel Limits ---
inline constexpr std::uint32_t MaxFrameBytes = 64U * 1024U * 1024U;
inline constexpr std::uint32_t FrameMaxDataBytes = 128U * 1024U * 1024U;

// --- Serialization & Protocol Decoding Limits ---
inline constexpr std::uint64_t MaxString = 1U * 1024U * 1024U;
inline constexpr std::size_t MaxDepth = 32;
// zpp applies this limit to each decoded container allocation, not to the
// aggregate allocation of a complete message.
inline constexpr std::size_t MaxContainerAllocationBytes = 128U * 1024U * 1024U;

// --- Rendering & Geometry Limits ---
inline constexpr int MaxRenderDimension = 16'384;
// Tiled renders allocate only the requested tile. Keep the virtual canvas
// within the signed coordinate range and leave room for MuPDF's one-pixel
// tile bleed when calculating the bounding box.
inline constexpr int MaxTiledRenderDimension = std::numeric_limits<std::int32_t>::max() - 2;

// --- EPUB / Content Limits ---
inline constexpr std::size_t MaxEpubCustomCssCharacters = 1000;
inline constexpr std::size_t MaxEpubCustomCssBase64Bytes = 8192;

// --- Annotation Geometry & Extras Limits ---
inline constexpr std::size_t MaxAnnotationPoints = 10'000;
inline constexpr std::size_t MaxAnnotationQuads = 10'000;
inline constexpr std::size_t MaxAnnotationInkPaths = 10'000;
inline constexpr std::size_t MaxAnnotationCalloutPoints = 3;
inline constexpr std::size_t MaxAnnotationInkPoints = 100'000;

// --- Annotation Metadata Extension Limits ---
inline constexpr std::size_t MaxAnnotationExtensionDepth = 16;
inline constexpr std::size_t MaxAnnotationExtensionEntries = 10'000;

// --- Form Fields Limits ---
inline constexpr std::size_t MaxPageFormFields = 10'000;
inline constexpr std::size_t MaxOpenFormFields = 100'000;
inline constexpr std::size_t MaxFormFieldStringBytes = 64 * 1024;
inline constexpr std::size_t MaxAggregateFormTextBytes = 4 * 1024 * 1024;
inline constexpr std::size_t MaxFormChoices = 10'000;
inline constexpr std::size_t MaxFormSelectedIndices = 1'000;
inline constexpr std::size_t MaxFormFieldHandleBytes = 128;
inline constexpr std::size_t MaxFormNameBytes = 1024;

} // namespace Mu::Limit

#endif // MUPDF_SHARED_PROTOCOL_LIMITS_HPP
