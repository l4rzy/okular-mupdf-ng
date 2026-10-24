// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/preview_goto_action.hpp"

#include "generator/conversion/document.hpp"
#include "generator/ui/link_preview.hpp"

namespace Mu::Generator::Proxy {

PreviewGotoAction::PreviewGotoAction(const Model::Viewport& destination, LinkPreview* previews, int sourcePage)
    : GotoAction(QString(), Conversion::viewportFromModel(destination))
    , m_destination(destination)
    , m_previews(previews)
    , m_sourcePage(sourcePage)
{
}

QString PreviewGotoAction::actionTip() const
{
    return m_previews && m_previews->showPreview(m_destination, m_sourcePage) ? QString() : GotoAction::actionTip();
}

} // namespace Mu::Generator::Proxy
