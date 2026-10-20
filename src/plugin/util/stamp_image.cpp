// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/stamp_image.hpp"
#include "shared/protocol/limits.hpp"

#include <QBuffer>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QStringList>

namespace Mu::Plugin::Util {

std::optional<std::vector<std::uint8_t>> prepareStampImage(const QString& symbol)
{
    static const QStringList nativeStamps {
        "Approved", "AsIs",     "Confidential", "Departmental",     "Draft",       "Experimental",
        "Expired",  "Final",    "ForComment",   "ForPublicRelease", "NotApproved", "NotForPublicRelease",
        "Sold",     "TopSecret"
    };
    if (symbol.isEmpty() || nativeStamps.contains(symbol))
        return std::vector<std::uint8_t> { };

    const int dimension = static_cast<int>(Limit::MaxStampImageDimension);
    const QSize bounds(dimension, dimension);
    QImage image;
    if (QFileInfo::exists(symbol)) {
        QImageReader reader(symbol);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        // Bound source decoding as well as the final image: not every image
        // plugin supports decoding directly at the requested reduced size.
        constexpr qint64 maxSourcePixels = 16 * 1024 * 1024;
        if (!size.isValid() || qint64(size.width()) * size.height() > maxSourcePixels)
            return std::nullopt;
        if (size.width() > dimension || size.height() > dimension)
            reader.setScaledSize(size.scaled(bounds, Qt::KeepAspectRatio));
        image = reader.read();
    } else {
        image = QIcon::fromTheme(symbol).pixmap(bounds).toImage();
    }
    if (image.isNull())
        return std::nullopt;
    if (image.width() > dimension || image.height() > dimension)
        image = image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")
        || static_cast<std::size_t>(png.size()) > Limit::MaxStampImageBytes)
        return std::nullopt;
    return std::vector<std::uint8_t>(png.begin(), png.end());
}

} // namespace Mu::Plugin::Util
