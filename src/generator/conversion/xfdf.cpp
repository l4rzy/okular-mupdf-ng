// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/xfdf.hpp"

#include "generator/conversion/annotation.hpp"
#include "plugin/util/stamp_image.hpp"
#include "plugin/xfdf/export.hpp"

#include <QTransform>

#include <algorithm>
#include <utility>

namespace Mu::Generator::Conversion {

namespace {

QSizeF pageSizePoints(const Okular::Page& page, const QSizeF& dpi)
{
    const bool sideways = page.rotation() == Okular::Rotation90 || page.rotation() == Okular::Rotation270;
    return QSizeF((sideways ? page.height() : page.width()) * (dpi.width() > 0 ? 72.0 / dpi.width() : 1.0),
                  (sideways ? page.width() : page.height()) * (dpi.height() > 0 ? 72.0 / dpi.height() : 1.0));
}

} // namespace

QString annotationsToXfdf(const QVector<Okular::Page*>& pages, const QSizeF& dpi)
{
    QVector<Plugin::Xfdf::Page> modelPages;
    modelPages.reserve(pages.size());

    // Okular pages carry device-pixel dimensions (points scaled by dpi/72); the
    // serializer needs PDF points so scaled annotation coordinates land on the
    // PDF user-space grid, matching the CLI export.
    for (const Okular::Page* page : pages) {
        Plugin::Xfdf::Page modelPage;
        if (page) {
            // Annotation geometry is untransformed, but page dimensions
            // follow the user's view rotation. Undo its width/height swap.
            const auto size = pageSizePoints(*page, dpi);
            modelPage.widthPoints = size.width();
            modelPage.heightPoints = size.height();
            for (const Okular::Annotation* annotation : page->annotations()) {
                if (const auto model = toModel(annotation))
                    modelPage.annotations.append(*model);
            }
        }
        modelPages.append(std::move(modelPage));
    }

    return Plugin::Xfdf::annotationsToXfdf(modelPages);
}

Plugin::Xfdf::XfdfParseResult
annotationsFromXfdf(const QByteArray& xml, const QVector<Okular::Page*>& pages, const QSizeF& dpi, QString* error)
{
    QVector<QSizeF> sizes;
    Plugin::Xfdf::XfdfParseLimits limits;
    std::size_t remaining = Limit::MaxAnnotationsPerDocument;
    for (const auto* page : pages) {
        sizes.append(page ? pageSizePoints(*page, dpi) : QSizeF());
        const auto count = page ? static_cast<std::size_t>(page->annotations().size()) : 0;
        remaining -= std::min(remaining, count);
        limits.annotationsPerPageRemaining.append(
            page ? Limit::MaxAnnotationsPerPage - std::min(Limit::MaxAnnotationsPerPage, count) : 0);
    }
    limits.annotationsRemaining = remaining;
    return Plugin::Xfdf::xfdfToAnnotations(xml, sizes, error, limits);
}

std::unique_ptr<Okular::Annotation> annotationFromXfdf(Model::Annotation annotation, Okular::Rotation rotation)
{
    // Imported symbols are untrusted PDF names, never host file paths/icons.
    if (annotation.subtype == Model::AnnotationType::Stamp && annotation.extras.style.appearance
        && !Plugin::Util::isNativeStamp(QString::fromStdString(annotation.extras.style.appearance->icon)))
        return nullptr;
    QTransform matrix;
    switch (rotation) {
    case Okular::Rotation90:
        matrix = QTransform(0, 1, -1, 0, 1, 0);
        break;
    case Okular::Rotation180:
        matrix = QTransform(-1, 0, 0, -1, 1, 1);
        break;
    case Okular::Rotation270:
        matrix = QTransform(0, -1, 1, 0, 0, 1);
        break;
    default:
        break;
    }
    const auto rect =
        matrix.mapRect(QRectF(QPointF(annotation.x0, annotation.y0), QPointF(annotation.x1, annotation.y1)));
    annotation.x0 = rect.left();
    annotation.y0 = rect.top();
    annotation.x1 = rect.right();
    annotation.y1 = rect.bottom();
    const auto rotatePoint = [&matrix](Model::Point& point) {
        const auto rotated = matrix.map(QPointF(point.x, point.y));
        point.x = rotated.x();
        point.y = rotated.y();
    };
    for (auto& point : annotation.extras.points)
        rotatePoint(point);
    // Okular's addition path transforms a highlight quad's display points,
    // but leaves its original point() geometry intact. Keep those canonical.
    for (auto& path : annotation.extras.inkPaths)
        for (auto& point : path)
            rotatePoint(point);
    for (auto& point : annotation.extras.callout)
        rotatePoint(point);
    // Import appends new annotations; source IDs must never alias existing notes.
    annotation.uuid.clear();
    annotation.handle.clear();
    auto result = fromModel(annotation);
    if (result)
        result->setFlags(result->flags() & ~Okular::Annotation::ExternallyDrawn);
    return result;
}

} // namespace Mu::Generator::Conversion
