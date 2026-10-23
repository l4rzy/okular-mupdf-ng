// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_LINK_PREVIEW_HPP
#define MU_GENERATOR_LINK_PREVIEW_HPP

#include "shared/model/types.hpp"

#include <QCache>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QSizeF>
#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>

namespace Mu::Generator {

/// GUI-thread owner of document-scoped, static internal-link tooltips.
class LinkPreview final : public QObject {
public:
    using Cancelled = std::function<bool()>;
    using Renderer = std::function<QImage(int, QSize, QRect, const Cancelled&)>;
    using PageSize = std::function<QSizeF(int)>;

    LinkPreview(Renderer render, PageSize pageSize);
    ~LinkPreview() override;

    QString tip(const Model::Viewport& destination);
    // Cancel and join before changing the worker document or destroying it.
    void reset();

private:
    Renderer m_render;
    PageSize m_pageSize;
    QThreadPool m_pool;
    QCache<QString, QString> m_cache { 4 * 1024 * 1024 };
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::uint64_t m_generation = 0;
    QString m_lastTip;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_LINK_PREVIEW_HPP
