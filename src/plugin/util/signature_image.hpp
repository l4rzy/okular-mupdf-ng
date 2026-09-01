// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_SIGNATURE_IMAGE_HPP
#define MU_PLUGIN_UTIL_SIGNATURE_IMAGE_HPP

#include <cstdint>
#include <vector>

#include <QString>

namespace Mu::Plugin::Util::SignatureImage {

/**
 * Reads a background image from disk, scales it proportionally to the signature widget rectangle,
 * and encodes it as a high-compression PNG byte buffer.
 *
 * @param path Filesystem path to the background image.
 * @param rectWidth Normalized width of the signature widget rectangle (0 falls back to a default extent).
 * @param rectHeight Normalized height of the signature widget rectangle (0 falls back to a default extent).
 * @param pageWidth Width of the target document page in points (default 595 pt).
 * @param pageHeight Height of the target document page in points (default 842 pt).
 * @return In-memory PNG byte buffer, or empty vector if the path is invalid or unreadable.
 */
[[nodiscard]] std::vector<std::uint8_t> prepareBackgroundImage(const QString& path,
                                                               double rectWidth = 0.0,
                                                               double rectHeight = 0.0,
                                                               double pageWidth = 595.0,
                                                               double pageHeight = 842.0);

} // namespace Mu::Plugin::Util::SignatureImage

#endif // MU_PLUGIN_UTIL_SIGNATURE_IMAGE_HPP
