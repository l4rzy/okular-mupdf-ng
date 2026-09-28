// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_XFDF_EXPORT_HPP
#define MU_PLUGIN_XFDF_EXPORT_HPP

#include <QString>
#include <QVector>

#include "plugin/xfdf/types.hpp"

namespace Mu::Plugin::Xfdf {

/// Serializes supported annotations as one UTF-8 XFDF XML document.
[[nodiscard]] QString annotationsToXfdf(const QVector<Page>& pages);

/// Converts an annotation's tight bounds to a PDF user-space rectangle. Bounds
/// are derived from the annotation's own geometry (quad/point/ink/callout union)
/// when present, falling back to the model rectangle otherwise.
[[nodiscard]] Model::Quad
normalizedRectToUserSpace(const Model::Annotation& annotation, double pageWidth, double pageHeight);

} // namespace Mu::Plugin::Xfdf

#endif // MU_PLUGIN_XFDF_EXPORT_HPP
