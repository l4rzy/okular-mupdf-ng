// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/types.hpp"
#include <QImage>
#include <QPixmap>
#include <okular/core/observer.h>

namespace Mu::Generator::Page {

// Pixmap loading background without changing Okular's accessibility settings.
// Never register this observer with Document: its pixmaps are only cross-observer
// previews, and must not acquire real render requests during refreshPixmaps().
// All methods run on the generator's GUI thread, including partialImage().
class Background final : public Okular::DocumentObserver {
public:
    void setTheme(Model::ReadingTheme theme);
    void seed(Okular::Page& page);
    /// Restores missing/rotated previews and removes late previews after reset.
    void refresh(Okular::Page& page);
    [[nodiscard]] QImage partialImage(QSize size) const;

private:
    QColor m_color;
    QPixmap m_preview;
    QPixmap m_rotatedPreview;
};

} // namespace Mu::Generator::Page
