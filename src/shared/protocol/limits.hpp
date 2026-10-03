// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_PROTOCOL_LIMITS_HPP
#define MU_SHARED_PROTOCOL_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace Mu::Limit {

// --- Control-Channel Serialized Message Limits ---
inline constexpr std::uint32_t MaxControlMessageBytes = 64U * 1024U * 1024U;

// --- Shared-Memory Render Frame Limits ---
inline constexpr std::uint32_t MaxSharedFrameBytes = 128U * 1024U * 1024U;
// Upper bound on concurrently live render-frame slots. The worker never creates
// more than this per session; the plugin refuses to cache beyond it, so a
// hostile worker cannot grow host memory. Keep both sides on this one value.
inline constexpr std::size_t MaxFrameSlotCount = 8;

// --- Serialization & Protocol Decoding Limits ---
inline constexpr std::uint64_t MaxString = 1024U * 1024U;
inline constexpr std::size_t MaxDepth = 32;
// zpp applies this limit to each decoded container allocation, not to the
// aggregate allocation of a complete message.
inline constexpr std::size_t MaxContainerAllocationBytes = 32U * 1024U * 1024U;

// --- Rendering & Geometry Limits ---
inline constexpr int MaxRenderDimension = 16'384;
// Tiled renders allocate only the requested tile. Capped at 64K (4x the
// single-page limit) to bound validation and height*stride arithmetic while
// still allowing large virtual canvases; reserve 2px for MuPDF's one-pixel
// tile bleed when calculating the bounding box.
inline constexpr int MaxTiledRenderDimension = 65'534;

// --- Resolution Limits ---
/// Minimum DPI accepted for render and OCR requests (MuPDF default resolution).
inline constexpr double MinDpi = 72.0;
/// Maximum DPI accepted for render and OCR requests.
inline constexpr double MaxDpi = 600.0;

// --- Worker Settings Limits ---
inline constexpr std::int32_t MaxDocumentAntialiasing = 8;
inline constexpr std::int32_t MaxDocumentImageQuality = 2;
inline constexpr std::int64_t MinDocumentMemoryCacheBytes = 32LL * 1024 * 1024;
inline constexpr std::int64_t MaxDocumentMemoryCacheBytes = 256LL * 1024 * 1024;

// --- EPUB / Content Limits ---
inline constexpr std::int32_t MinEpubFontSize = 10;
inline constexpr std::int32_t MaxEpubFontSize = 20;
inline constexpr std::size_t MaxEpubCustomCssCharacters = 1000;
inline constexpr std::size_t MaxEpubCustomCssBase64Bytes = 8192;

// --- Annotation Geometry & Extras Limits ---
// Leave room for heavily marked-up pages and detailed freehand strokes.
inline constexpr std::size_t MaxAnnotationsPerPage = 2048;
inline constexpr std::size_t MaxAnnotationsPerDocument = 32'768;
inline constexpr std::size_t MaxAnnotationPoints = 4096;
inline constexpr std::size_t MaxAnnotationQuads = 4096;
inline constexpr std::size_t MaxAnnotationInkPaths = 1024;
inline constexpr std::size_t MaxAnnotationCalloutPoints = 3;
inline constexpr std::size_t MaxAnnotationInkPoints = 32'768;

// --- Annotation Metadata Extension Limits ---
inline constexpr std::size_t MaxAnnotationExtensionDepth = 8;
inline constexpr std::size_t MaxAnnotationExtensionEntries = 2048;

// --- PDF Layer UI Limits ---
inline constexpr std::size_t MaxLayerEntries = 8192;
inline constexpr std::int32_t MaxLayerDepth = 32;
inline constexpr std::size_t MaxLayerNameBytes = 2048;
inline constexpr std::size_t MaxLayerTextBytes = 256 * 1024;

// --- Form Fields Limits ---
// Generous bounds for ordinary interactive forms; counts include repeated widgets
// and the document text budget includes every copied name, value, and option,
// plus choice/export string objects (including empty options).
inline constexpr std::size_t MaxPageFormFields = 512;
inline constexpr std::size_t MaxOpenFormFields = 32'768;
inline constexpr std::size_t MaxFormFieldStringBytes = 64 * 1024;
inline constexpr std::size_t MaxAggregateFormTextBytes = 4 * 1024 * 1024;
inline constexpr std::size_t MaxFormChoices = 2048;
inline constexpr std::size_t MaxFormSelectedIndices = 256;
inline constexpr std::size_t MaxHandleBytes = 128;
inline constexpr std::size_t MaxFormFieldHandleBytes = MaxHandleBytes;
inline constexpr std::size_t MaxFormNameBytes = 1024;

} // namespace Mu::Limit

#endif // MU_SHARED_PROTOCOL_LIMITS_HPP
