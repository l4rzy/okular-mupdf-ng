// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QString>

namespace Mu::Generator {

enum class PlaceholderKind {
    Error,
    Loading,
};

/**
 * Creates an antialiased placeholder/status image for error or loading states.
 *
 * @param width Canvas width in pixels.
 * @param height Canvas height in pixels.
 * @param kind Category of placeholder (Error or Loading).
 * @param message Optional message override. If empty, localized default is used.
 * @return Generated QImage in Format_ARGB32_Premultiplied, or null QImage if dimensions <= 0.
 */
[[nodiscard]] QImage createPlaceholderImage(int width, int height, PlaceholderKind kind, const QString& message = { });

} // namespace Mu::Generator
