// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/xfdf.hpp"

#include "generator/conversion/annotation.hpp"
#include "plugin/xfdf/export.hpp"

#include <utility>

namespace Mu::Generator::Conversion {

QString annotationsToXfdf(const QVector<Okular::Page*>& pages, const QSizeF& dpi)
{
    QVector<Plugin::Xfdf::Page> modelPages;
    modelPages.reserve(pages.size());

    // Okular pages carry device-pixel dimensions (points scaled by dpi/72); the
    // serializer needs PDF points so scaled annotation coordinates land on the
    // PDF user-space grid, matching the CLI export.
    const double scaleX = dpi.width() > 0 ? 72.0 / dpi.width() : 1.0;
    const double scaleY = dpi.height() > 0 ? 72.0 / dpi.height() : 1.0;

    for (const Okular::Page* page : pages) {
        Plugin::Xfdf::Page modelPage;
        if (page) {
            // Annotation geometry is untransformed, but page dimensions
            // follow the user's view rotation. Undo its width/height swap.
            const bool sideways = page->rotation() == Okular::Rotation90 || page->rotation() == Okular::Rotation270;
            modelPage.widthPoints = (sideways ? page->height() : page->width()) * scaleX;
            modelPage.heightPoints = (sideways ? page->width() : page->height()) * scaleY;
            for (const Okular::Annotation* annotation : page->annotations()) {
                if (const auto model = toModel(annotation))
                    modelPage.annotations.append(*model);
            }
        }
        modelPages.append(std::move(modelPage));
    }

    return Plugin::Xfdf::annotationsToXfdf(modelPages);
}

} // namespace Mu::Generator::Conversion
