// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_XFDF_HPP
#define MU_PLUGIN_UTIL_XFDF_HPP

#include <QString>
#include <QVector>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Util {

/// A page of model annotations and the geometry used to serialize them.
///
/// Annotation coordinates arrive already normalized against the page's rotated
/// display bounds (top-left origin, Y down), so no rotation is applied here.
///
/// Known limitation: pages with a non-zero /Rotate are serialized in their
/// rotated display frame rather than unrotated PDF user-space, so coordinates
/// are displaced for rotated pages.
struct XfdfPage {
    double widthPoints = 0;
    double heightPoints = 0;
    QVector<Model::Annotation> annotations;
};

/// Serializes supported annotations as one UTF-8 XFDF XML document.
[[nodiscard]] QString annotationsToXfdf(const QVector<XfdfPage>& pages);

/// Converts an annotation's tight bounds to a PDF user-space rectangle. Bounds
/// are derived from the annotation's own geometry (quad/point/ink/callout union)
/// when present, falling back to the model rectangle otherwise.
[[nodiscard]] Model::Quad
normalizedRectToUserSpace(const Model::Annotation& annotation, double pageWidth, double pageHeight);

} // namespace Mu::Plugin::Util

#endif // MU_PLUGIN_UTIL_XFDF_HPP
