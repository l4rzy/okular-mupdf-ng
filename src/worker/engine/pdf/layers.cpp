// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <cstring>

extern "C" {
#include <mupdf/pdf.h>
}

#include "shared/protocol/limits.hpp"

namespace Mu::Worker::Engine {

std::vector<Model::LayerEntry> PdfDocument::layers(std::string* error) const
{
    std::vector<Model::LayerEntry> result;
    // Keep owning containers outside MuPDF's longjmp exception scope.
    std::vector<int> parentDepths;
    if (!m_document || m_locked) {
        fail(error, "document is unavailable");
        return result;
    }
    pdf_document* pdf = pdf_specifics(m_context, m_document);
    fz_try(m_context)
    {
        const int count = pdf_count_layer_config_ui(m_context, pdf);
        if (count < 0 || static_cast<std::size_t>(count) > Limit::MaxLayerEntries)
            fz_throw(m_context, FZ_ERROR_LIMIT, "layer entry limit exceeded");
        result.reserve(static_cast<std::size_t>(count));
        std::size_t textBytes = 0;
        for (int id = 0; id < count; ++id) {
            pdf_layer_config_ui info { };
            pdf_layer_config_ui_info(m_context, pdf, id, &info);
            const char* name = info.text ? info.text : "";
            const auto size = strnlen(name, Limit::MaxLayerNameBytes + 1);
            textBytes += size;
            if (size > Limit::MaxLayerNameBytes || textBytes > Limit::MaxLayerTextBytes || info.depth < 0
                || info.depth > Limit::MaxLayerDepth)
                fz_throw(m_context, FZ_ERROR_LIMIT, "layer UI limit exceeded");
            Model::LayerType type = Model::LayerType::Label;
            if (info.type == PDF_LAYER_UI_CHECKBOX)
                type = Model::LayerType::CheckBox;
            else if (info.type == PDF_LAYER_UI_RADIOBOX)
                type = Model::LayerType::RadioButton;
            // Unnamed nested arrays need not have a visible parent row.
            while (!parentDepths.empty() && parentDepths.back() >= info.depth)
                parentDepths.pop_back();
            const int depth = static_cast<int>(parentDepths.size());
            parentDepths.push_back(info.depth);
            result.push_back({ id, depth, name, type, info.selected != 0, info.locked != 0 });
        }
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        result.clear();
    }
    return result;
}

bool PdfDocument::setLayer(std::int32_t id, bool selected, std::string* error)
{
    // Validate the complete UI before any change, including all resource bounds.
    std::string lookupError;
    const auto entries = layers(&lookupError);
    if (!lookupError.empty())
        return fail(error, lookupError);
    if (id < 0 || static_cast<std::size_t>(id) >= entries.size())
        return fail(error, "layer entry is out of range");
    const auto& entry = entries[static_cast<std::size_t>(id)];
    if (entry.type == Model::LayerType::Label || entry.locked)
        return fail(error, "layer entry cannot be changed");
    if (entry.selected == selected)
        return true;
    pdf_document* pdf = pdf_specifics(m_context, m_document);
    fz_try(m_context)
    {
        if (selected)
            pdf_select_layer_config_ui(m_context, pdf, id);
        else
            pdf_deselect_layer_config_ui(m_context, pdf, id);
        // Visibility is runtime state; do not write it into the PDF defaults.
        clearPageCache();
    }
    fz_catch(m_context)
    {
        return fail(error, fz_caught_message(m_context));
    }
    return true;
}

} // namespace Mu::Worker::Engine
