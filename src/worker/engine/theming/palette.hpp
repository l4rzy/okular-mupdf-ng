// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/types.hpp"
#include <cstdint>
#include <mupdf/fitz.h>

namespace Mu::Worker::Engine {

std::uint32_t readingPaperColor(const Model::DocumentSettings& settings) noexcept;

bool isMostlyDarkPixmap(fz_context* context, fz_pixmap* pixmap);
bool isMostlyDarkPage(
    fz_context* context, fz_page* page, fz_rect bounds, fz_cookie* cookie, fz_display_list* list = nullptr);

// Dim premultiplied RGBA to 80%, retaining alpha.
void dimImageRow(unsigned char* pixel, int width) noexcept;

// Transform only the fully composited viewing pixmap, leaving alpha unchanged.
void applyDocumentTheme(fz_context* context, fz_pixmap* pixmap, Model::ReadingTheme theme, const fz_cookie* cookie);

} // namespace Mu::Worker::Engine
