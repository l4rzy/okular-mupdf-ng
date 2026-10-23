// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/link_preview.hpp"

#include <KLocalizedString>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace Mu::Generator {

class PreviewScrollArea final : public QScrollArea {
public:
    explicit PreviewScrollArea(QWidget* parent)
        : QScrollArea(parent)
    {
        viewport()->setCursor(Qt::OpenHandCursor);
    }

    bool isDragging() const { return m_dragPosition.has_value(); }

protected:
    bool viewportEvent(QEvent* event) override
    {
        if (event->type() == QEvent::UngrabMouse)
            cancelDrag();
        return QScrollArea::viewportEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QScrollArea::mousePressEvent(event);
            return;
        }
        m_dragPosition = event->globalPosition().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_dragPosition && !(event->buttons() & Qt::LeftButton))
            cancelDrag();
        if (!m_dragPosition) {
            QScrollArea::mouseMoveEvent(event);
            return;
        }
        // Global coordinates remain stable as the image moves under the pointer.
        const QPoint position = event->globalPosition().toPoint();
        const QPoint delta = *m_dragPosition - position;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() + delta.y());
        m_dragPosition = position;
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !m_dragPosition) {
            QScrollArea::mouseReleaseEvent(event);
            return;
        }
        cancelDrag();
        event->accept();
    }

    void hideEvent(QHideEvent* event) override
    {
        cancelDrag();
        QScrollArea::hideEvent(event);
    }

private:
    void cancelDrag()
    {
        m_dragPosition.reset();
        viewport()->setCursor(Qt::OpenHandCursor);
    }

    std::optional<QPoint> m_dragPosition;
};

class LinkPreviewWindow final : public QFrame {
public:
    explicit LinkPreviewWindow(QWidget* parent)
        : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
    {
        setObjectName(QStringLiteral("mupdfLinkPreview"));
        setWindowTitle(i18n("Link preview"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        // Tool windows otherwise use the application palette instead of the
        // owning Okular window's color scheme. Inherit it, including live changes.
        setAttribute(Qt::WA_WindowPropagation);
        // Only pixels outside the rounded outline are transparent.
        setAttribute(Qt::WA_TranslucentBackground);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        m_status->setForegroundRole(QPalette::WindowText);
        layout->addWidget(m_status);
        m_scroll = new PreviewScrollArea(this);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->viewport()->setBackgroundRole(QPalette::Window);
        m_image = new QLabel;
        m_image->setBackgroundRole(QPalette::Window);
        m_image->setObjectName(QStringLiteral("previewImage"));
        m_scroll->setWidget(m_image);
        layout->addWidget(m_scroll);
    }

    bool isDragging() const { return m_scroll->isDragging(); }

    void prepare(int page, LinkPreviewSize size, bool loading, const QRect& available, QPoint anchor)
    {
        m_status->setText(loading ? i18n("Page %1 — Loading preview…", static_cast<qint64>(page) + 1)
                                  : i18n("Page %1", static_cast<qint64>(page) + 1));
        if (loading) {
            m_image->clear();
            m_image->resize(size.render.width(), size.visibleHeight);
        }
        m_scroll->setFixedWidth(size.render.width());
        ensurePolished();
        resize(std::min(available.width(), sizeHint().width()), std::min(available.height(), size.visibleHeight + 48));
        const int x = anchor.x() + 12 + width() <= available.right() + 1 ? anchor.x() + 12 : anchor.x() - width() - 12;
        const int y =
            anchor.y() + 20 + height() <= available.bottom() + 1 ? anchor.y() + 20 : anchor.y() - height() - 12;
        move(std::clamp(x, available.left(), std::max(available.left(), available.right() - width() + 1)),
             std::clamp(y, available.top(), std::max(available.top(), available.bottom() - height() + 1)));
    }

    void showUnavailable() { m_status->setText(i18n("Preview unavailable")); }

    void setPages(int pageNumber, const QImage& pages, const QRect& target, double targetY)
    {
        m_status->setText(i18n("Page %1", static_cast<qint64>(pageNumber) + 1));
        m_image->setPixmap(QPixmap::fromImage(pages));
        m_image->resize(pages.size());
        m_scroll->verticalScrollBar()->setValue(
            std::clamp(target.top() + static_cast<int>(targetY * target.height()) - 24,
                       0,
                       m_scroll->verticalScrollBar()->maximum()));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto opaqueColor = [this](QPalette::ColorRole role) {
            QColor color = palette().color(role);
            color.setAlpha(255);
            return color;
        };
        painter.setPen(QPen(opaqueColor(QPalette::Mid), 1));
        painter.setBrush(opaqueColor(QPalette::Window));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }

private:
    QLabel* m_status;
    PreviewScrollArea* m_scroll;
    QLabel* m_image;
};

LinkPreview::LinkPreview(Renderer render, PageInfo pageInfo)
    : m_render(std::move(render))
    , m_pageInfo(std::move(pageInfo))
{
    m_pool.setMaxThreadCount(1);
    m_closeTimer.setSingleShot(true);
    m_closeTimer.setInterval(350);
    connect(&m_closeTimer, &QTimer::timeout, this, [this] {
        if (!isHovered())
            dismiss();
    });
}

LinkPreview::~LinkPreview()
{
    reset();
}

void LinkPreview::dismiss()
{
    m_closeTimer.stop();
    qApp->removeEventFilter(this);
    if (m_cancelled)
        m_cancelled->store(true);
    if (m_window)
        m_window->hide();
    m_source.clear();
}

void LinkPreview::reset()
{
    dismiss();
    ++m_generation;
    m_pool.waitForDone();
    m_cancelled.reset();
    m_cache.clear();
    delete m_window.data();
}

void LinkPreview::setMode(LinkPreviewMode mode)
{
    if (m_mode == mode)
        return;
    reset();
    m_mode = mode;
}

bool LinkPreview::showPreview(const Model::Viewport& destination)
{
    if (m_mode == LinkPreviewMode::Off || destination.page < 0)
        return false;
    auto* source = QApplication::widgetAt(QCursor::pos());
    if (!source)
        return false;
    if (m_window && (source == m_window.data() || m_window->isAncestorOf(source)))
        return true; // A delayed source tooltip must not replace the open preview.
    auto* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = source->screen();
    if (!screen)
        return false;
    const QRect available = screen->availableGeometry();
    const auto sizeForPage = [&](int page) {
        return sizeLinkPreview(m_mode, source->window()->size(), available.size(), m_pageInfo(page));
    };
    LinkPreviewSize previewSize = sizeForPage(destination.page);
    if (previewSize.render.isEmpty())
        return false;
    std::vector<RenderPage> pages;
    QString key = QString::number(destination.page);
    for (int offset : { -1, 0, 1 }) {
        if ((offset < 0 && destination.page == 0)
            || (offset > 0 && destination.page == std::numeric_limits<int>::max()))
            continue;
        const int page = destination.page + offset;
        const QSize size = offset == 0 ? previewSize.render : sizeForPage(page).render;
        if (size.isEmpty())
            continue;
        pages.push_back({ page, size });
        key += QStringLiteral(":%1:%2:%3").arg(page).arg(size.width()).arg(size.height());
    }
    // Neighbors can have different page dimensions. Fit the widest page.
    for (const auto& page : pages)
        previewSize.render.setWidth(std::max(previewSize.render.width(), page.size.width()));
    const auto* cached = m_cache.object(key);
    if (!cached && m_cancelled && (key != m_key || m_cancelled->load())) {
        dismiss();
        return false; // Keep at most one render in flight; never queue hover jobs.
    }

    m_source = source;
    m_anchor = QCursor::pos();
    m_key = key;
    m_targetY =
        (destination.coordinateMask & Model::Viewport::CoordinateY) != 0 && std::isfinite(destination.normalizedY)
        ? std::clamp(destination.normalizedY, 0.0, 1.0)
        : 0.0;
    if (m_window && m_window->parentWidget() != source->window())
        delete m_window.data();
    if (!m_window)
        m_window = new LinkPreviewWindow(source->window());
    m_window->setScreen(screen);
    m_window->prepare(destination.page, previewSize, !cached, available, m_anchor);
    m_closeTimer.stop();
    QToolTip::hideText();
    qApp->installEventFilter(this);
    m_window->show();
    if (cached) {
        m_window->setPages(destination.page, cached->image, cached->target, m_targetY);
        return true;
    }
    if (m_cancelled)
        return true;

    startRender(destination.page, std::move(pages), key);
    return true;
}

void LinkPreview::startRender(int page, std::vector<RenderPage> pages, const QString& key)
{
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    const auto generation = m_generation;
    const auto render = m_render;
    m_pool.start([this, render, cancelled, generation, key, page, pages = std::move(pages)] {
        constexpr int PageGap = 8;
        std::vector<QImage> images;
        int width = 0;
        int height = 0;
        bool hasTarget = false;
        for (const auto& request : pages) {
            if (cancelled->load())
                break;
            QImage image = render(request.number, request.size, [cancelled] { return cancelled->load(); });
            // Worker images lease shared frame slots. Copy before requesting
            // the next page so this strip cannot exhaust the frame pool.
            image = !cancelled->load() && image.size() == request.size ? image.copy() : QImage { };
            if (!image.isNull()) {
                width = std::max(width, image.width());
                height += image.height() + PageGap;
                hasTarget |= request.number == page;
            }
            images.push_back(std::move(image));
        }
        RenderedPages result;
        if (hasTarget && !cancelled->load()) {
            result.image = QImage(width, height - PageGap, QImage::Format_ARGB32_Premultiplied);
            result.image.fill(Qt::transparent); // Page gaps inherit the preview's theme.
            QPainter painter(&result.image);
            int top = 0;
            for (std::size_t i = 0; i < images.size(); ++i) {
                const QImage& image = images[i];
                if (image.isNull())
                    continue; // An unavailable neighbor must not hide the destination.
                const QRect rect(QPoint((width - image.width()) / 2, top), image.size());
                painter.drawImage(rect.topLeft(), image);
                if (pages[i].number == page)
                    result.target = rect;
                top += image.height() + PageGap;
            }
        }
        QMetaObject::invokeMethod(this, [this, cancelled, generation, key, page, result = std::move(result)] {
            if (generation != m_generation)
                return;
            m_cancelled.reset();
            if (cancelled->load())
                return;
            if (result.image.isNull()) {
                if (m_window && key == m_key)
                    m_window->showUnavailable();
                return;
            }
            m_cache.insert(
                key, new RenderedPages(result), static_cast<int>((result.image.sizeInBytes() + 1023) / 1024));
            if (m_window && m_window->isVisible() && key == m_key)
                m_window->setPages(page, result.image, result.target, m_targetY);
        });
    });
}

bool LinkPreview::isHovered() const
{
    if (!m_window || !m_window->isVisible() || !m_source || !m_source->isVisible())
        return false;
    if (m_window->isDragging() && (QApplication::mouseButtons() & Qt::LeftButton))
        return true;
    const QPoint position = QCursor::pos();
    return m_window->frameGeometry().contains(position)
        || (QApplication::widgetAt(position) == m_source
            && QRect(m_anchor - QPoint(4, 4), QSize(9, 9)).contains(position));
}

bool LinkPreview::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_window || !m_window->isVisible())
        return false;
    const auto* widget = qobject_cast<QWidget*>(watched);
    const bool inPreview = watched == m_window->windowHandle()
        || (widget && (widget == m_window.data() || m_window->isAncestorOf(widget)));
    switch (event->type()) {
    case QEvent::MouseButtonRelease:
    case QEvent::UngrabMouse:
        // Input reaches this filter before the scroll area clears its drag
        // state. Check hover after that transition, at the grace deadline.
        m_closeTimer.start();
        break;
    case QEvent::MouseMove:
    case QEvent::Enter:
    case QEvent::Leave:
        if (isHovered())
            m_closeTimer.stop();
        else if (!m_closeTimer.isActive())
            m_closeTimer.start();
        break;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            dismiss();
            return true;
        }
        [[fallthrough]];
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
        if (!inPreview)
            dismiss();
        break;
    case QEvent::Hide:
    case QEvent::WindowDeactivate:
        if (m_source && (watched == m_source || watched == m_source->window()))
            dismiss();
        break;
    default:
        break;
    }
    return false;
}

} // namespace Mu::Generator
