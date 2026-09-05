// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_MUPDF_HELPERS_HPP
#define MU_WORKER_ENGINE_MUPDF_HELPERS_HPP

#include <cstddef>

extern "C" {
#include <mupdf/fitz.h>
}

#include "shared/model/types.hpp"

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace Mu::Worker::Engine {

// MuPDF exceptions use setjmp/longjmp. Keep fz_try blocks narrow and avoid
// introducing RAII objects after the jump point. Declare C++ output objects
// before the boundary when practical, reset partial output in fz_catch, call
// fz_var for mutable state consumed after a jump, and keep MuPDF ownership
// cleanup in fz_always blocks. Do not add mirror model structs solely to make a
// block look like C: the boundary should remain small enough to audit directly.

/// Counts total characters inside a MuPDF structured text page.
inline std::size_t countStextChars(const fz_stext_page* text) noexcept
{
    if (!text)
        return 0;
    std::size_t count = 0;
    for (const fz_stext_block* block = text->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT)
            continue;
        for (const fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
            for (const fz_stext_char* character = line->first_char; character; character = character->next) {
                ++count;
            }
        }
    }
    return count;
}

/// Releases cached store entries and trims unused malloc arenas back to OS.
inline void trimProcessMemory(fz_context* context) noexcept
{
    if (context) {
        fz_empty_store(context);
    }
#if defined(__linux__) && defined(__GLIBC__)
    malloc_trim(0);
#endif
}

/// Applies configuration and rendering settings to a Fitz context.
inline void applyFitzSettings(fz_context* context, const ::Mu::Model::DocumentSettings& settings) noexcept
{
    if (!context)
        return;

    fz_set_graphics_aa_level(context, settings.graphicsAntialiasing);
    fz_set_text_aa_level(context, settings.textAntialiasing);
    fz_tune_image_rendering(context, settings.imageQuality);
}

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_MUPDF_HELPERS_HPP
