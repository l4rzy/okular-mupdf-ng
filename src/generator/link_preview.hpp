// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_LINK_PREVIEW_HPP
#define MU_GENERATOR_LINK_PREVIEW_HPP

#include "generator/link_preview_policy.hpp"
#include "shared/model/types.hpp"

#include <QCache>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QWidget;

namespace Mu::Generator {

class LinkPreviewWindow;

/// GUI-thread owner of document-scoped, scrollable internal-link previews.
class LinkPreview final : public QObject {
public:
    using Cancelled = std::function<bool()>;
    using Renderer = std::function<QImage(int, QSize, const Cancelled&)>;
    // Return empty geometry for pages outside the current document.
    using PageInfo = std::function<LinkPreviewPage(int)>;

    LinkPreview(Renderer render, PageInfo pageInfo);
    ~LinkPreview() override;

    // Called by the link's tooltip hook; true suppresses Okular's text tooltip.
    bool showPreview(const Model::Viewport& destination);
    // Cancel and join before changing the worker document or destroying it.
    void reset();
    void setMode(LinkPreviewMode mode);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool isHovered() const;
    void dismiss();

    enum class PageState { Pending, Ready, Failed };

    struct RenderPage {
        int number;
        QRect rect;
        PageState state = PageState::Pending;
    };

    void renderVisiblePages();
    void startRender(std::size_t index);
    static QString pageKey(const RenderPage& page);

    Renderer m_render;
    PageInfo m_pageInfo;
    LinkPreviewMode m_mode = LinkPreviewMode::Off;
    QThreadPool m_pool;
    QCache<QString, QImage> m_cache { 16 * 1024 }; // KiB, excluding the displayed pixmap.
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::uint64_t m_generation = 0;
    QPointer<LinkPreviewWindow> m_window;
    QPointer<QWidget> m_source;
    QPoint m_anchor;
    QString m_key;
    std::vector<RenderPage> m_pages;
    std::size_t m_targetIndex = 0;
    QTimer m_closeTimer;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_LINK_PREVIEW_HPP
