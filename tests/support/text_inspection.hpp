// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <mupdf/fitz.h>

namespace Mu::Test {

struct TextSample {
    float size;
    fz_point origin;
};

struct TextInspection {
    QMap<QString, TextSample> lines;
    QList<fz_rect> images;
    QString error;
};

// Call after layout. Copies text metrics so no MuPDF objects escape the document.
inline TextInspection inspectDocumentText(fz_context* context, fz_document* document)
{
    TextInspection result;
    fz_stext_page* text = nullptr;
    fz_var(result);
    fz_var(text);
    fz_try(context)
    {
        for (int page = 0; page < fz_count_pages(context, document); ++page) {
            fz_stext_options options { };
            options.flags = FZ_STEXT_PRESERVE_IMAGES;
            text = fz_new_stext_page_from_page_number(context, document, page, &options);
            for (auto* block = text->first_block; block; block = block->next) {
                if (block->type == FZ_STEXT_BLOCK_IMAGE)
                    result.images.append(block->bbox);
                if (block->type != FZ_STEXT_BLOCK_TEXT)
                    continue;
                for (auto* line = block->u.t.first_line; line; line = line->next) {
                    if (!line->first_char)
                        continue;
                    QString label;
                    for (auto* ch = line->first_char; ch; ch = ch->next) {
                        const auto code = static_cast<char32_t>(ch->c);
                        label += QString::fromUcs4(&code, 1);
                    }
                    result.lines.insert(label, { line->first_char->size, line->first_char->origin });
                }
            }
            fz_drop_stext_page(context, text);
            text = nullptr;
        }
    }
    fz_always(context)
    {
        fz_drop_stext_page(context, text);
    }
    fz_catch(context)
    {
        result.error = QString::fromUtf8(fz_caught_message(context));
    }
    return result;
}

} // namespace Mu::Test
