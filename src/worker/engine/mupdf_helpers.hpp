// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_MUPDF_HELPERS_HPP
#define MU_WORKER_ENGINE_MUPDF_HELPERS_HPP

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
}

#include "shared/model/types.hpp"

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace Mu::Worker::Engine {

/// Formats an epoch timestamp as a compact user-facing signature date,
/// e.g. "Sep 7, 2026 13:14 CDT", using the process local timezone.
/// Fallback for requests without a plugin-provided display date.
inline std::string formatSignatureDate(std::int64_t epochSeconds)
{
    const std::time_t time = static_cast<std::time_t>(epochSeconds);
    std::tm broken { };
    ::localtime_r(&time, &broken);
    char formatted[64] { };
    const std::size_t length = std::strftime(formatted, sizeof formatted, "%b %d, %Y %H:%M %Z", &broken);
    if (length == 0)
        return { };
    std::string result(formatted, length);
    // Strip the leading zero of single-digit days: "Sep 07" -> "Sep 7".
    if (result.size() > 4 && result[3] == ' ' && result[4] == '0')
        result.erase(4, 1);
    return result;
}

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

/// Trims the MuPDF store toward storePercent of its current size, evicting
/// eligible entries, and returns freed heap pages to the OS. Idle-only:
/// never call on the render hot path.
inline void trimIdleStore(fz_context* context, unsigned int storePercent) noexcept
{
    if (context) {
        fz_try(context)
        {
            (void)fz_shrink_store(context, storePercent);
        }
        fz_catch(context)
        {
        }
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
