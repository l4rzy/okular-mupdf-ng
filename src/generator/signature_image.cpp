// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/signature_image.hpp"

#include <algorithm>

#include <QBuffer>
#include <QFile>
#include <QImageReader>

namespace Mu::Generator::SignatureImage {

// =============================================================================
// Background Image File Loading & PNG Encoding
// =============================================================================

std::vector<std::uint8_t>
prepareBackgroundImage(const QString& path, const Okular::NormalizedRect& rect, double pageWidth, double pageHeight)
{
    // Signature images are imported into generated documents, so keep input
    // reads bounded and return an empty result for any unreadable source.
    if (path.isEmpty() || !QFile::exists(path))
        return { };

    // Calculate target image pixel dimensions based on page point dimensions
    // and normalized bounding box (2x factor for retina/HiDPI rendering)
    double width = (pageWidth > 0 ? pageWidth : 595.0) * (rect.width() > 0 ? rect.width() : 0.3) * 2.0;
    double height = (pageHeight > 0 ? pageHeight : 842.0) * (rect.height() > 0 ? rect.height() : 0.1) * 2.0;

    // Constrain to standard signature stamp pixel bounds to prevent PDF bloat
    const int targetWidth = std::clamp(static_cast<int>(width), 64, 384);
    const int targetHeight = std::clamp(static_cast<int>(height), 32, 256);

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
    // Use PNG compression quality 9 (maximum zlib compression)
    if (!buffer.open(QIODevice::WriteOnly) || !scaled.save(&buffer, "PNG", 9))
        return { };

    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(pngBytes.constData()),
                                     reinterpret_cast<const std::uint8_t*>(pngBytes.constData()) + pngBytes.size());
}

} // namespace Mu::Generator::SignatureImage
