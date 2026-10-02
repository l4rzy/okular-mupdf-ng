// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_CONSTANTS_HPP
#define MU_WORKER_ENGINE_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>

#include "shared/protocol/limits.hpp"

namespace Mu::Worker::Engine::Constant {

// --- Document Cache & Store Limits ---
inline constexpr std::size_t DefaultStoreSize = 64ULL * 1024ULL * 1024ULL;

// --- Page Cache ---
/// Number of parsed page handles kept for reuse across renders.
inline constexpr std::size_t PageCacheSize = 3;

// --- Rendering Constants ---
inline constexpr int TileBleed = 1;
/// PDF points per inch used for DPI scaling (MuPDF default resolution).
inline constexpr double PointsPerInch = 72.0;

// --- Time Conversions ---
/// Milliseconds per second for Unix epoch conversions.
inline constexpr std::int64_t MillisecondsPerSecond = 1000;

// --- Unicode Validity ---
inline constexpr int UnicodeMaxCodePoint = 0x10FFFF;
inline constexpr int UnicodeSurrogateMin = 0xD800;
inline constexpr int UnicodeSurrogateMax = 0xDFFF;

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
inline constexpr int MaxEmbeddedTreeDepth = 32;
inline constexpr std::size_t MaxEmbeddedTreeEntries = 1'000;

// --- PDF Annotations & Signatures ---
inline constexpr const char* SignatureAppearanceFontFileName = "Allura-Regular.ttf";
inline constexpr int MaxAnnotationGeometryPoints = 4'096;
inline constexpr std::size_t MaxPageAnnotations = ::Mu::Limit::MaxAnnotationsPerPage;
inline constexpr std::size_t MaxPageSignatures = 100'000;
inline constexpr std::size_t MaxSignatureCmsBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t MaxPageSignatureCmsBytes = 32U * 1024U * 1024U;

// --- OCR Engine ---
inline constexpr std::size_t MaxOcrBoxes = 200'000;
/// 60-second watchdog as 600 ticks of 100 ms.
inline constexpr int OcrWatchdogTicks = 600;
inline constexpr int OcrWatchdogTickMs = 100;

// --- PKCS#7 & Digital Signing ---
inline constexpr std::size_t MaxPkcs7SignatureBufferBytes = 64U * 1024U;
inline constexpr std::size_t DigestStreamingChunkBytes = 65'536;
inline constexpr std::size_t FileCopyChunkBytes = 65'536;
/// Mask for O(1) 6-digit hex signature widget suffix.
inline constexpr std::uint32_t SignatureNameSuffixMask = 0xFFFFFF;

// --- Sandbox Resource Limits ---
inline constexpr std::size_t SandboxAddressSpaceBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr int SandboxCpuSoftSeconds = 60;
inline constexpr int SandboxCpuHardSeconds = 120;

} // namespace Mu::Worker::Engine::Constant

#endif // MU_WORKER_ENGINE_CONSTANTS_HPP
