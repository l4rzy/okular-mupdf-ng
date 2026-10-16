// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/types.hpp"

class QColor;

namespace Mu::Generator {

// Legacy fallback for SDKs without Page::setPlaceholderColor().
// Instances share ownership of Okular's process-wide accessibility settings.
class PageColorOverride {
public:
    PageColorOverride() = default;
    ~PageColorOverride();
    PageColorOverride(const PageColorOverride&) = delete;
    PageColorOverride& operator=(const PageColorOverride&) = delete;

    void setTheme(Model::ReadingTheme theme);
    void reset();
    // Worker paper color must never inherit the temporary placeholder color.
    [[nodiscard]] static QColor originalPaperColor();

private:
    bool m_active = false;
};

} // namespace Mu::Generator
