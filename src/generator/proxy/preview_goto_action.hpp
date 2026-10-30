// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_PREVIEW_GOTO_ACTION_HPP
#define MU_GENERATOR_PROXY_PREVIEW_GOTO_ACTION_HPP

#include "shared/model/types.hpp"

#include <okular/core/action.h>

#include <QPointer>

namespace Mu::Generator {

class LinkPreview;

}

namespace Mu::Generator::Proxy {

/// Internal navigation action that delegates hover previews to the UI owner.
class PreviewGotoAction final : public Okular::GotoAction {
public:
    PreviewGotoAction(const Model::Viewport& destination, LinkPreview* previews, int sourcePage);

    PreviewGotoAction(const QString& uri, LinkPreview* previews, int sourcePage);
    QString actionTip() const override;

private:
    Model::Viewport m_destination;
    QPointer<LinkPreview> m_previews;
    int m_sourcePage;
};

} // namespace Mu::Generator::Proxy

#endif // MU_GENERATOR_PROXY_PREVIEW_GOTO_ACTION_HPP
