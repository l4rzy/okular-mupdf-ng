// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/render_image.hpp"

namespace Mu::Plugin::Util {

QImage normalizeRenderImage(QImage source, const QSize& expectedSize)
{
    if (source.isNull() || !expectedSize.isValid() || source.size() == expectedSize)
        return source;
    return source.scaled(expectedSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace Mu::Plugin::Util
