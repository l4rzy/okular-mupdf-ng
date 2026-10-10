// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_XFDF_TYPES_HPP
#define MU_PLUGIN_XFDF_TYPES_HPP

#include <QVector>

#include "shared/model/types.hpp"

namespace Mu::Plugin::Xfdf {

/// A page of model annotations and the geometry used to serialize them.
///
/// Annotation coordinates arrive already normalized against the page's rotated
/// display bounds (top-left origin, Y down), so no rotation is applied here.
///
/// Callers must reject PDFs containing rotated pages: this representation
/// cannot convert their display coordinates to unrotated PDF user-space.
struct Page {
    double widthPoints = 0;
    double heightPoints = 0;
    QVector<Model::Annotation> annotations;
};

} // namespace Mu::Plugin::Xfdf

#endif // MU_PLUGIN_XFDF_TYPES_HPP
