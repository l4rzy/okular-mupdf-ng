// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_RENDER_IMAGE_HPP
#define MU_PLUGIN_UTIL_RENDER_IMAGE_HPP

#include <QImage>
#include <QSize>

namespace Mu::Plugin::Util {

/**
 * Returns @p source resized to @p expectedSize when they differ.
 *
 * Exact-sized and null images pass through unchanged so the common render
 * path stays zero-copy; fitted or edge-clipped frames are resampled with
 * smooth transformation. An invalid @p expectedSize returns @p source.
 */
[[nodiscard]] QImage normalizeRenderImage(QImage source, const QSize& expectedSize);

} // namespace Mu::Plugin::Util

#endif // MU_PLUGIN_UTIL_RENDER_IMAGE_HPP
