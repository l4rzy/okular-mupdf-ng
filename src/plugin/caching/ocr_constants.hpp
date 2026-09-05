// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_CACHING_OCR_CONSTANTS_HPP
#define MU_PLUGIN_CACHING_OCR_CONSTANTS_HPP

#include <QtGlobal>

namespace Mu::Plugin::OCR::Constant {

// Cache binary format and resource bounds. These limits apply before
// decompression as well as after it, preventing malformed cache files from
// causing unbounded allocation or item processing.
constexpr quint32 CACHE_MAGIC = 0x4F435232; // 'OCR2' (Uncompressed 8-byte header prefix)
constexpr qsizetype CACHE_HEADER_BYTES = 8;
constexpr quint32 MAX_ITEMS_PER_PAGE = 1000000;

constexpr qint64 MAX_CACHE_COMPRESSED_BYTES = 8LL * 1024LL * 1024LL;
constexpr qint64 MAX_CACHE_RAW_BYTES = 64LL * 1024LL * 1024LL;

constexpr float DPI_FAST = 150.0f;
constexpr float DPI_BALANCED = 225.0f;
constexpr float DPI_ACCURACY = 300.0f;

} // namespace Mu::Plugin::OCR::Constant
#endif // MU_PLUGIN_CACHING_OCR_CONSTANTS_HPP
