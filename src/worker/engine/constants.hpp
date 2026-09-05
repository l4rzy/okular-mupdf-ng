// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_CONSTANTS_HPP
#define MU_WORKER_ENGINE_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>

namespace Mu::Worker::Engine::Constant {

// --- Document Cache & Store Limits ---
inline constexpr std::size_t DefaultStoreSize = 64ULL * 1024ULL * 1024ULL;

// --- Rendering Constants ---
inline constexpr int TileBleed = 1;

// --- EPUB Layout & Geometry ---
inline constexpr float MillimetersToPoints = 72.0f / 25.4f;
inline constexpr float PageMarginFraction = 0.05f;
inline constexpr const char* EpubPdfWriterOptions =
    "compress=yes,compress-images=yes,compress-fonts=yes,garbage=deduplicate,objstms=yes";

// --- Outline & Link Hierarchy ---
inline constexpr std::size_t MaxOutlineDepth = 64;
inline constexpr std::size_t MaxOutlineNodes = 100'000;
inline constexpr std::size_t MaxEpubOutlineNodes = 50'000;
inline constexpr std::size_t MaxPageLinks = 100'000;

// --- Link Resolution Cache ---
inline constexpr std::size_t MaxResolvedLinkCacheEntries = 8'192;
inline constexpr std::size_t MaxResolvedLinkCacheKeyBytes = 4U * 1024U * 1024U;
inline constexpr float DestinationTopMarginPoints = 16.0f;

// --- PDF Embedded Files ---
inline constexpr std::size_t MaxEmbeddedBytes = 16U * 1024U * 1024U;

// --- PDF Annotations & Signatures ---
inline constexpr int MaxAnnotationGeometryPoints = 10'000;
inline constexpr std::size_t MaxPageAnnotations = 100'000;
inline constexpr std::size_t MaxPageSignatures = 100'000;
inline constexpr std::size_t MaxSignatureCmsBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t MaxPageSignatureCmsBytes = 32U * 1024U * 1024U;

// --- OCR Engine ---
inline constexpr std::size_t MaxOcrBoxes = 200'000;

// --- PKCS#7 & Digital Signing ---
inline constexpr std::size_t MaxPkcs7SignatureBufferBytes = 64U * 1024U;
inline constexpr std::size_t DigestStreamingChunkBytes = 65'536;
inline constexpr std::size_t FileCopyChunkBytes = 65'536;

} // namespace Mu::Worker::Engine::Constant

#endif // MU_WORKER_ENGINE_CONSTANTS_HPP
