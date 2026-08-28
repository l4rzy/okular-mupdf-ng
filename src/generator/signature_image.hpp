// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_GENERATOR_SIGNATURE_IMAGE_HPP
#define MUPDF_GENERATOR_SIGNATURE_IMAGE_HPP

#include <cstdint>
#include <vector>

#include <QString>

#include <okular/core/area.h>

namespace Mu::Generator::SignatureImage {

/**
 * Reads a background image from disk, scales it proportionally to the signature widget rectangle,
 * and encodes it as a high-compression PNG byte buffer.
 *
 * @param path Filesystem path to the background image.
 * @param rect Normalized bounding rectangle of the signature widget (optional).
 * @param pageWidth Width of the target document page in points (default 595 pt).
 * @param pageHeight Height of the target document page in points (default 842 pt).
 * @return In-memory PNG byte buffer, or empty vector if the path is invalid or unreadable.
 */
[[nodiscard]] std::vector<std::uint8_t> prepareBackgroundImage(const QString& path,
                                                               const Okular::NormalizedRect& rect = { },
                                                               double pageWidth = 595.0,
                                                               double pageHeight = 842.0);

} // namespace Mu::Generator::SignatureImage

#endif // MUPDF_GENERATOR_SIGNATURE_IMAGE_HPP
