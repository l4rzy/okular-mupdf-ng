// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONVERSION_XFDF_HPP
#define MU_GENERATOR_CONVERSION_XFDF_HPP

#include <QSizeF>
#include <QString>
#include <QVector>

#include <okular/core/page.h>

#include "plugin/xfdf/import.hpp"

#include <memory>

namespace Mu::Generator::Conversion {

/// Serializes every annotation on the given pages to a single XFDF document.
///
/// Converts live annotations to Mu::Model values and delegates serialization to
/// the Qt-based plugin utility. Unsupported subtypes and signatures are skipped.
/// Okular pages report their size in device pixels, so @p dpi is used to
/// convert those dimensions back to PDF points.
///
/// The export entry point must reject PDFs containing rotated pages before
/// calling this adapter (see Plugin::Xfdf::Page).
[[nodiscard]] QString annotationsToXfdf(const QVector<Okular::Page*>& pages, const QSizeF& dpi);

/// Parses against the live page sizes and remaining annotation capacity.
[[nodiscard]] Plugin::Xfdf::XfdfParseResult
annotationsFromXfdf(const QByteArray& xml, const QVector<Okular::Page*>& pages, const QSizeF& dpi, QString* error);

/// Prepares fresh, view-rotated geometry for Document::addPageAnnotation(),
/// which transforms it back into the page's unrotated coordinate space.
[[nodiscard]] std::unique_ptr<Okular::Annotation> annotationFromXfdf(Model::Annotation annotation,
                                                                     Okular::Rotation rotation);

} // namespace Mu::Generator::Conversion

#endif // MU_GENERATOR_CONVERSION_XFDF_HPP
