// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_SIGNATURE_IMAGE_HPP
#define MU_PLUGIN_UTIL_SIGNATURE_IMAGE_HPP

#include <cstdint>
#include <vector>

#include <QString>

namespace Mu::Plugin::Util::SignatureImage {

namespace Constant {

/// Fallback A4 page size in points when the caller passes no page geometry.
inline constexpr double DefaultPageWidthPt = 595.0;
inline constexpr double DefaultPageHeightPt = 842.0;
/// Fallback normalized widget extents when the caller passes no rectangle.
inline constexpr double DefaultRectWidth = 0.3;
inline constexpr double DefaultRectHeight = 0.1;
/// Rendering scale for retina/HiDPI output.
inline constexpr double HiDpiScale = 2.0;
/// Stamp pixel bounds constraining the generated PNG.
inline constexpr int MinWidth = 64;
inline constexpr int MaxWidth = 384;
inline constexpr int MinHeight = 32;
inline constexpr int MaxHeight = 256;
/// Maximum zlib compression for the generated PNG.
inline constexpr int PngCompressionQuality = 9;

} // namespace Constant

/**
 * Reads a background image from disk, scales it proportionally to the signature widget rectangle,
 * and encodes it as a high-compression PNG byte buffer.
 *
 * @param path Filesystem path to the background image.
 * @param rectWidth Normalized width of the signature widget rectangle (0 falls back to a default extent).
 * @param rectHeight Normalized height of the signature widget rectangle (0 falls back to a default extent).
 * @param pageWidth Width of the target document page in points (default A4 595 pt).
 * @param pageHeight Height of the target document page in points (default A4 842 pt).
 * @return In-memory PNG byte buffer, or empty vector if the path is invalid or unreadable.
 */
[[nodiscard]] std::vector<std::uint8_t> prepareBackgroundImage(const QString& path,
                                                               double rectWidth = 0.0,
                                                               double rectHeight = 0.0,
                                                               double pageWidth = Constant::DefaultPageWidthPt,
                                                               double pageHeight = Constant::DefaultPageHeightPt);

} // namespace Mu::Plugin::Util::SignatureImage

#endif // MU_PLUGIN_UTIL_SIGNATURE_IMAGE_HPP
