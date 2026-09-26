// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/util/signature_image.hpp"

#include <algorithm>

#include <QBuffer>
#include <QFile>
#include <QImageReader>

namespace Mu::Plugin::Util::SignatureImage {

// =============================================================================
// Background Image File Loading & PNG Encoding
// =============================================================================

std::vector<std::uint8_t>
prepareBackgroundImage(const QString& path, double rectWidth, double rectHeight, double pageWidth, double pageHeight)
{
    // Signature images are imported into generated documents, so keep input
    // reads bounded and return an empty result for any unreadable source.
    if (path.isEmpty() || !QFile::exists(path))
        return { };

    // Calculate target image pixel dimensions based on page point dimensions
    // and normalized bounding box extent (HiDPI factor for retina rendering)
    const double width = (pageWidth > 0 ? pageWidth : Constant::DefaultPageWidthPt)
        * (rectWidth > 0 ? rectWidth : Constant::DefaultRectWidth) * Constant::HiDpiScale;
    const double height = (pageHeight > 0 ? pageHeight : Constant::DefaultPageHeightPt)
        * (rectHeight > 0 ? rectHeight : Constant::DefaultRectHeight) * Constant::HiDpiScale;

    // Constrain to standard signature stamp pixel bounds to prevent PDF bloat
    const int targetWidth = std::clamp(static_cast<int>(width), Constant::MinWidth, Constant::MaxWidth);
    const int targetHeight = std::clamp(static_cast<int>(height), Constant::MinHeight, Constant::MaxHeight);

    QImageReader reader(path);
    const QSize imageSize = reader.size();
    if (!imageSize.isNull() && imageSize.width() > 0 && imageSize.height() > 0) {
        reader.setScaledSize(imageSize.scaled(targetWidth, targetHeight, Qt::KeepAspectRatio));
    }

    const QImage input = reader.read();
    if (input.isNull())
        return { };

    // Scale with smooth anti-aliasing to target bounds
    const QImage scaled = input.scaled(targetWidth, targetHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray pngBytes;
    QBuffer buffer(&pngBytes);
    if (!buffer.open(QIODevice::WriteOnly) || !scaled.save(&buffer, "PNG", Constant::PngCompressionQuality))
        return { };

    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(pngBytes.constData()),
                                     reinterpret_cast<const std::uint8_t*>(pngBytes.constData()) + pngBytes.size());
}

} // namespace Mu::Plugin::Util::SignatureImage
