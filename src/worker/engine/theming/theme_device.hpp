// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shared/model/types.hpp"
#include <mupdf/fitz.h>

namespace Mu::Worker::Engine {

// Recolors vector marks and processes images during viewing, preserving alpha and masks.
// Keeps a reference to target; the caller retains ownership of its reference.
fz_device* newThemeDevice(fz_context* context,
                          fz_device* target,
                          fz_matrix transform,
                          fz_irect bounds,
                          const fz_cookie* cookie,
                          Model::ReadingTheme theme,
                          bool recolorVectors = true);

} // namespace Mu::Worker::Engine
