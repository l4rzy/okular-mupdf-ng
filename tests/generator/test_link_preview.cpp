// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/document.hpp"
#include "generator/ui/link_preview.hpp"
#include "plugin/worker_client.hpp"

#include <QApplication>
#include <QCursor>
#include <QLabel>
#include <QLayout>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <QtTest/qtestwheel.h>
#include <okular/core/page.h>

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

using namespace Mu;
using Generator::LinkPreviewMode;
using Generator::LinkPreviewPage;

namespace {

Model::Viewport destination(double y = 0)
{
    Model::Viewport viewport;
    viewport.page = 0;
    viewport.normalizedY = y;
    viewport.coordinateMask = Model::Viewport::CoordinateY;
    return viewport;
}

auto pageRange(int count, QSizeF size = QSizeF(640, 960))
{
    return [count, size](int page) {
        return LinkPreviewPage { page >= 0 && page < count ? size : QSizeF { } };
    };
}

auto singlePage(QSizeF size)
{
    return pageRange(1, size);
}

QImage solidPage(QSize size, QColor color = Qt::red)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

QWidget* previewWindow()
{
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QStringLiteral("mupdfLinkPreview") && widget->isVisible())
            return widget;
    return nullptr;
}

QScrollArea* previewScrollArea()
{
    auto* window = previewWindow();
    return window ? window->findChild<QScrollArea*>() : nullptr;
}

QImage previewImage()
{
    const auto* window = previewWindow();
    const auto* label = window ? window->findChild<QLabel*>(QStringLiteral("previewImage")) : nullptr;
    return label ? label->pixmap().toImage() : QImage { };
}

QLabel* previewPage(int number)
{
    auto* window = previewWindow();
    if (!window)
        return nullptr;
    for (auto* label : window->findChildren<QLabel*>())
        if (label->property("pageNumber").isValid() && label->property("pageNumber").toInt() == number)
            return label;
    return nullptr;
}

int scrollToPage(int page)
{
    auto* bar = previewScrollArea()->verticalScrollBar();
    const int position = std::min(previewPage(page)->y(), bar->maximum());
    bar->setValue(position);
    return position;
}

QImage pageImage(int number)
{
    const auto* label = previewPage(number);
    return label ? label->pixmap().toImage() : QImage { };
}

void sendWheel(QWidget* target, int delta)
{
    const QPoint position = target->rect().center();
    QWheelEvent event(position,
                      target->mapToGlobal(position),
                      { },
                      QPoint(0, delta),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);
    QApplication::sendEvent(target, &event);
}

void flickPreview(QWidget* window, int direction)
{
    auto* viewport = window->findChild<QScrollArea*>()->viewport();
    const QPoint globalStart = viewport->mapToGlobal(viewport->rect().center());
    const QPoint start = window->mapFromGlobal(globalStart);
    // Keep the physical cursor inside the tooltip on the offscreen platform.
    QCursor::setPos(globalStart);
    QCoreApplication::processEvents();
    QTest::mousePress(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, start, 16);
    for (int distance : { 24, 64, 104 })
        QTest::mouseMove(window->windowHandle(), start + QPoint(0, direction * distance), 16);
    QTest::mouseRelease(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, start + QPoint(0, direction * 120), 16);
}

class Host final : public QWidget {
public:
    int wheels = 0;
    int clicks = 0;

    void wheelEvent(QWheelEvent*) override { ++wheels; }

    void mousePressEvent(QMouseEvent*) override { ++clicks; }
};

} // namespace

class TestLinkPreview : public QObject {
    Q_OBJECT
    Host m_host;

    QSize renderSize(QSizeF page = QSizeF(640, 960), LinkPreviewMode mode = LinkPreviewMode::Large) const
    {
        return Generator::sizeLinkPreview(mode, m_host.size(), m_host.screen()->geometry().size(), { page }).render;
    }

private slots:

    void init()
    {
        m_host.wheels = 0;
        m_host.clicks = 0;
        m_host.setGeometry(20, 20, 760, 560);
        m_host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_host));
        QTest::mouseMove(&m_host, QPoint(40, 40));
        QTRY_COMPARE(QApplication::widgetAt(QCursor::pos()), &m_host);
    }

    void sizesPreview_data()
    {
        QTest::addColumn<LinkPreviewMode>("mode");
        QTest::addColumn<QSize>("window");
        QTest::addColumn<QSize>("screen");
        QTest::addColumn<QSizeF>("page");
        QTest::addColumn<double>("scale");
        QTest::addColumn<QSize>("render");
        QTest::addColumn<int>("height");
        QTest::addColumn<QSize>("sessionScreen");
        const QSize window(1000, 800), screen(1920, 1080);
        const QSizeF page(600, 900);
        QTest::newRow("off") << LinkPreviewMode::Off << window << screen << page << 0.0 << QSize() << 0 << QSize();
        QTest::newRow("small") << LinkPreviewMode::Small << window << screen << page << 0.0 << QSize(468, 702) << 222
                               << QSize();
        QTest::newRow("large") << LinkPreviewMode::Large << window << screen << page << 0.0 << QSize(628, 942) << 312
                               << QSize();
        QTest::newRow("auto-pdf") << LinkPreviewMode::AdaptiveSize << window << screen << page << 0.0 << QSize(550, 825)
                                  << 440 << QSize();
        QTest::newRow("auto-window-bound")
            << LinkPreviewMode::AdaptiveSize << window << screen << page << 2.0 << QSize(700, 1050) << 440 << QSize();
        QTest::newRow("auto-preview-scale")
            << LinkPreviewMode::AdaptiveSize << window << screen << page << 1.0 << QSize(600, 900) << 440 << QSize();
        QTest::newRow("auto-zoomed-out") << LinkPreviewMode::AdaptiveSize << window << screen << page << 0.5
                                         << QSize(300, 450) << 440 << QSize();
        QTest::newRow("auto-large-window") << LinkPreviewMode::AdaptiveSize << QSize(2400, 1600) << QSize(2560, 1920)
                                           << page << 0.0 << QSize(1268, 1902) << 880 << QSize();
        QTest::newRow("screen-bound") << LinkPreviewMode::Large << window << QSize(320, 240) << page << 0.0
                                      << QSize(94, 141) << 32 << QSize();
        QTest::newRow("short-page") << LinkPreviewMode::Large << window << screen << QSizeF(1200, 300) << 0.0
                                    << QSize(628, 157) << 157 << QSize();
        QTest::newRow("wide-page") << LinkPreviewMode::Large << window << screen << QSizeF(1120, 560) << 0.0
                                   << QSize(628, 314) << 312 << QSize();
        QTest::newRow("extremely-wide-page") << LinkPreviewMode::Large << window << screen << QSizeF(1e300, 1e-300)
                                             << 0.0 << QSize(628, 1) << 1 << QSize();
        QTest::newRow("extremely-tall-page") << LinkPreviewMode::Large << window << screen << QSizeF(1e-300, 1e300)
                                             << 0.0 << QSize(1, 4096) << 312 << QSize();
        QTest::newRow("invalid-page") << LinkPreviewMode::Large << window << screen << QSizeF(0, 900) << 0.0 << QSize()
                                      << 0 << QSize();
        QTest::newRow("non-finite-page") << LinkPreviewMode::AdaptiveSize << window << screen
                                         << QSizeF(std::numeric_limits<double>::infinity(), 900) << 11.0 << QSize() << 0
                                         << QSize();
        QTest::newRow("invalid-scale") << LinkPreviewMode::AdaptiveSize << window << screen << page
                                       << std::numeric_limits<double>::quiet_NaN() << QSize(550, 825) << 440 << QSize();
        QTest::newRow("infinite-scale") << LinkPreviewMode::AdaptiveSize << window << screen << page
                                        << std::numeric_limits<double>::infinity() << QSize(550, 825) << 440 << QSize();
        QTest::newRow("negative-scale") << LinkPreviewMode::AdaptiveSize << window << screen << page << -1.0
                                        << QSize(550, 825) << 440 << QSize();
        QTest::newRow("auto-screen-bound") << LinkPreviewMode::AdaptiveSize << window << QSize(320, 240) << page << 2.0
                                           << QSize(148, 222) << 72 << QSize();
        QTest::newRow("auto-screen-height-bound") << LinkPreviewMode::AdaptiveSize << window << QSize(1920, 600) << page
                                                  << 2.0 << QSize(700, 1050) << 252 << QSize();
        QTest::newRow("auto-scaled-screen-bound") << LinkPreviewMode::AdaptiveSize << window << QSize(960, 540) << page
                                                  << 2.0 << QSize(468, 702) << 222 << QSize();
        QTest::newRow("auto-render-bound")
            << LinkPreviewMode::AdaptiveSize << QSize(10000, 10000) << QSize(10000, 10000) << QSizeF(600, 600) << 10.0
            << QSize(4096, 4096) << 4096 << QSize();
        const double tiny = std::numeric_limits<double>::denorm_min();
        QTest::newRow("subnormal-page") << LinkPreviewMode::AdaptiveSize << window << screen << QSizeF(tiny, tiny)
                                        << 0.0 << QSize(1, 1) << 1 << QSize();
        QTest::newRow("no-window") << LinkPreviewMode::AdaptiveSize << QSize() << screen << page << 11.0 << QSize() << 0
                                   << QSize();
        QTest::newRow("no-screen") << LinkPreviewMode::Large << window << QSize() << page << 0.0 << QSize() << 0
                                   << QSize();
        QTest::newRow("invalid-mode") << static_cast<LinkPreviewMode>(99) << window << screen << page << 0.0 << QSize()
                                      << 0 << QSize();
        QTest::newRow("large-on-larger-monitor") << LinkPreviewMode::Large << QSize() << QSize(3840, 2160) << page
                                                 << 0.0 << QSize(628, 942) << 312 << QSize(1920, 1080);
        QTest::newRow("small-from-larger-monitor") << LinkPreviewMode::Small << QSize() << screen << page << 0.0
                                                   << QSize(948, 1422) << 492 << QSize(3840, 2160);
        QTest::newRow("fit-small-monitor")
            << LinkPreviewMode::Small << QSize() << QSize(320, 240) << page << 0.0 << QSize(272, 408) << 192 << screen;
    }

    void sizesPreview()
    {
        QFETCH(LinkPreviewMode, mode);
        QFETCH(QSize, window);
        QFETCH(QSize, screen);
        QFETCH(QSizeF, page);
        QFETCH(double, scale);
        QFETCH(QSize, render);
        QFETCH(int, height);
        QFETCH(QSize, sessionScreen);
        const auto size = Generator::sizeLinkPreview(mode, window, screen, { page }, scale, sessionScreen);
        QCOMPARE(size.render, render);
        QCOMPARE(size.visibleHeight, height);
    }

    void defaultsToOff()
    {
        std::atomic_int renders = 0;
        int pageQueries = 0;
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto&) {
                ++renders;
                return solidPage(size);
            },
            [&](int) {
                ++pageQueries;
                return LinkPreviewPage { QSizeF(600, 900) };
            });
        QVERIFY(!previews.showPreview(destination()));
        QVERIFY(!previewWindow());
        QCOMPARE(pageQueries, 0);
        QCOMPARE(renders.load(), 0);
    }

    void presetsIgnoreZoomAndWindowSize_data()
    {
        QTest::addColumn<LinkPreviewMode>("mode");
        QTest::newRow("small") << LinkPreviewMode::Small;
        QTest::newRow("large") << LinkPreviewMode::Large;
    }

    void presetsIgnoreZoomAndWindowSize()
    {
        QFETCH(LinkPreviewMode, mode);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(600, 900)));
        previews.setMode(mode);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        const QSize imageSize = previewImage().size();
        const QSize popupSize = previewWindow()->size();
        const int divisor = mode == LinkPreviewMode::Small ? 4 : 3;
        QCOMPARE(popupSize.width(), m_host.screen()->geometry().width() / divisor);
        QVERIFY(popupSize.height() <= m_host.screen()->geometry().height() / divisor);
        QTest::keyClick(&m_host, Qt::Key_Escape);
        m_host.resize(500, 400);
        previews.observeRender(&m_host, 0, (QSizeF(120, 180) * m_host.devicePixelRatioF()).toSize());
        for (int attempt = 0; attempt < 2; ++attempt) {
            QVERIFY(previews.showPreview(destination()));
            QTRY_COMPARE(previewImage().size(), imageSize);
            QCOMPARE(previewWindow()->size(), popupSize);
            previews.setMode(LinkPreviewMode::Off);
            previews.reset();
            previews.setMode(mode);
        }
    }

    void autoUsesHoveredViewScale()
    {
        QWidget content(&m_host), thumbnail;
        content.setGeometry(0, 0, 200, 200);
        content.show();
        QTest::mouseMove(&content, QPoint(40, 40));
        QTRY_COMPARE(QApplication::widgetAt(QCursor::pos()), &content);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(600, 900)));
        // Track while Off; enabling the setting must retain the view's scale.
        previews.observeRender(&m_host, 0, (QSizeF(300, 450) * m_host.devicePixelRatioF()).toSize());
        previews.observeRender(&thumbnail, 0, QSize(60, 90));
        previews.observeRender(nullptr, 0, QSize(100, 150));
        previews.observeRender(&m_host, -1, QSize(100, 150));
        previews.observeRender(&m_host, 0, QSize());
        previews.setMode(LinkPreviewMode::AdaptiveSize);
        QVERIFY(previews.showPreview(destination()));
        QTRY_COMPARE(previewImage().size(), QSize(210, 315));
        QTest::keyClick(&content, Qt::Key_Escape);
        // A subsequent main-view render updates the next preview.
        previews.observeRender(&m_host, 0, (QSizeF(400, 600) * m_host.devicePixelRatioF()).toSize());
        QVERIFY(previews.showPreview(destination()));
        QTRY_COMPARE(previewImage().size(), QSize(280, 420));
        previews.reset();
        QVERIFY(previews.showPreview(destination()));
        // A new document must not inherit the old document's reading scale.
        QTRY_COMPARE(previewImage().width(),
                     std::min(qRound(m_host.width() * 0.55), m_host.screen()->geometry().width() / 2 - 12));
    }

    void autoUsesSourcePageScale_data()
    {
        QTest::addColumn<int>("sourcePage");
        QTest::addColumn<int>("expectedWidth");
        QTest::newRow("updated-page") << 0 << 126;
        QTest::newRow("other-page") << 1 << 168;
        QTest::newRow("latest-without-source") << -1 << 126;
    }

    void autoUsesSourcePageScale()
    {
        QFETCH(int, sourcePage);
        QFETCH(int, expectedWidth);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        pageRange(2, QSizeF(600, 900)));
        for (const auto& [page, width] : { std::pair { 0, 120 }, { 1, 240 }, { 0, 180 } })
            previews.observeRender(&m_host, page, (QSizeF(width, width * 1.5) * m_host.devicePixelRatioF()).toSize());
        previews.setMode(LinkPreviewMode::AdaptiveSize);
        QVERIFY(previews.showPreview(destination(), sourcePage));
        QTRY_COMPARE(previewImage().width(), expectedWidth);
    }

    void destroyedViewDiscardsScale()
    {
        // Reuse the same address deterministically to detect stale pointer keys.
        std::optional<QWidget> view;
        view.emplace(&m_host);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(600, 900)));
        previews.observeRender(&*view, 0, (QSizeF(120, 180) * view->devicePixelRatioF()).toSize());
        view.reset();
        view.emplace(&m_host);
        view->setGeometry(0, 0, 200, 200);
        view->show();
        QTest::mouseMove(&*view, QPoint(40, 40));
        QTRY_COMPARE(QApplication::widgetAt(QCursor::pos()), &*view);
        previews.setMode(LinkPreviewMode::AdaptiveSize);
        QVERIFY(previews.showPreview(destination(), 0));
        QTRY_COMPARE(previewImage().width(),
                     std::min(qRound(m_host.width() * 0.55), m_host.screen()->geometry().width() / 2 - 12));
        QTest::keyClick(&*view, Qt::Key_Escape);
        previews.observeRender(&*view, 0, (QSizeF(120, 180) * view->devicePixelRatioF()).toSize());
        QVERIFY(previews.showPreview(destination(), 0));
        QTRY_COMPARE(previewImage().width(), 84);
    }

    void autoSharesScaleWithNeighbors()
    {
        Generator::LinkPreview previews(
            [](int, QSize size, const auto&) { return solidPage(size); },
            [](int page) {
                return LinkPreviewPage { page == 0 ? QSizeF(600, 900) : page == 1 ? QSizeF(1200, 900) : QSizeF { } };
            });
        previews.observeRender(&m_host, 0, (QSizeF(600, 900) * m_host.devicePixelRatioF()).toSize());
        previews.setMode(LinkPreviewMode::AdaptiveSize);
        QVERIFY(previews.showPreview(destination()));
        const int maxWidth = std::min(qRound(m_host.width() * 0.70), m_host.screen()->geometry().width() / 2 - 12);
        QTRY_COMPARE(previewImage().size(), QSize(qRound(maxWidth / 2.0), qRound(maxWidth * 0.75)));
        auto* neighbor = previewWindow()->findChild<QLabel*>(QStringLiteral("previewPage"));
        QVERIFY(neighbor);
        QCOMPARE(neighbor->size(), QSize(maxWidth, qRound(maxWidth * 0.75)));
    }

    void autoShrinksContainerAfterZoomOut_data()
    {
        QTest::addColumn<bool>("dismissFirst");
        QTest::newRow("visible-popup") << false;
        QTest::newRow("reopened-popup") << true;
    }

    void autoShrinksContainerAfterZoomOut()
    {
        QFETCH(bool, dismissFirst);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(600, 900)));
        previews.setMode(LinkPreviewMode::AdaptiveSize);
        previews.observeRender(&m_host, 0, (QSizeF(1200, 1800) * m_host.devicePixelRatioF()).toSize());
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        const int previousWidth = previewWindow()->width();
        if (dismissFirst)
            QTest::keyClick(&m_host, Qt::Key_Escape);
        previews.observeRender(&m_host, 0, (QSizeF(240, 360) * m_host.devicePixelRatioF()).toSize());
        QVERIFY(previews.showPreview(destination()));
        QTRY_COMPARE(previewImage().size(), QSize(168, 252));
        auto* window = previewWindow();
        auto* scroll = previewScrollArea();
        QVERIFY(scroll);
        const auto margins = window->layout()->contentsMargins();
        QCOMPARE(window->width(), 168 + margins.left() + margins.right());
        QCOMPARE(scroll->viewport()->width(), 168);
        QVERIFY(window->width() < previousWidth);
    }

    void headerRespectsPopupHeight_data()
    {
        QTest::addColumn<LinkPreviewMode>("mode");
        QTest::addColumn<int>("fontSize");
        QTest::newRow("small") << LinkPreviewMode::Small << 72;
        QTest::newRow("large") << LinkPreviewMode::Large << 120;
        QTest::newRow("adaptive") << LinkPreviewMode::AdaptiveSize << 72;
    }

    void headerRespectsPopupHeight()
    {
        QFETCH(LinkPreviewMode, mode);
        QFETCH(int, fontSize);
        const QFont original = QApplication::font();
        const auto restoreFont = qScopeGuard([&] { QApplication::setFont(original); });
        QFont font = original;
        font.setPointSize(fontSize);
        QApplication::setFont(font);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(600, 900)));
        previews.setMode(mode);
        previews.observeRender(&m_host, 0, (QSizeF(120, 180) * m_host.devicePixelRatioF()).toSize());
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        const auto size = Generator::sizeLinkPreview(
            mode, m_host.size(), m_host.screen()->geometry().size(), { QSizeF(600, 900) }, 0.14);
        QVERIFY(previewWindow()->height() <= size.visibleHeight + 48);
        QVERIFY(previewScrollArea()->viewport()->height() > 0);
    }

    void modeChangeCancelsRender()
    {
        std::atomic_int calls = 0;
        std::atomic_bool started = false;
        std::atomic_bool finished = false;
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto& cancelled) {
                if (++calls == 1) {
                    started = true;
                    while (!cancelled())
                        QThread::msleep(1);
                    finished = true;
                    return solidPage(size, Qt::red);
                }
                return solidPage(size, Qt::blue);
            },
            singlePage(QSizeF(600, 900)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(started.load());
        previews.setMode(LinkPreviewMode::Off);
        QVERIFY(finished.load());
        QVERIFY(!previewWindow());
        QVERIFY(!previews.showPreview(destination()));
        previews.setMode(LinkPreviewMode::Small);
        QVERIFY(previews.showPreview(destination()));
        QTRY_COMPARE(previewImage().size(), renderSize(QSizeF(600, 900), LinkPreviewMode::Small));
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::blue));
        QCOMPARE(calls.load(), 2);
    }

    void autoFollowsWindowSize_data()
    {
        QTest::addColumn<double>("scale");
        QTest::newRow("before-first-render") << 0.0;
        QTest::newRow("reading-scale") << 1.0;
        QTest::newRow("zoomed-in") << 2.0;
    }

    void autoFollowsWindowSize()
    {
        QFETCH(double, scale);
        std::atomic_int calls = 0;
        const LinkPreviewPage page { QSizeF(560, 840) };
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto&) {
                ++calls;
                return solidPage(size);
            },
            [page](int index) { return index == 0 ? page : LinkPreviewPage { }; });
        if (scale > 0)
            previews.observeRender(&m_host, 0, (page.size * (scale * m_host.devicePixelRatioF())).toSize());
        previews.setMode(LinkPreviewMode::AdaptiveSize);
        const auto screen = m_host.screen()->geometry().size();
        QVERIFY(previews.showPreview(destination()));
        const auto initial =
            Generator::sizeLinkPreview(LinkPreviewMode::AdaptiveSize, m_host.size(), screen, page, scale * 0.70);
        QTRY_COMPARE(previewImage().size(), initial.render);
        QVERIFY(previewWindow()->height() <= initial.visibleHeight + 48);
        QVERIFY(previewWindow()->width() <= screen.width() / 2);
        QVERIFY(previewWindow()->height() <= screen.height() / 2);
        m_host.resize(500, 400);
        QTest::mouseMove(&m_host, QPoint(40, 40));
        QVERIFY(previews.showPreview(destination()));
        const auto resized =
            Generator::sizeLinkPreview(LinkPreviewMode::AdaptiveSize, m_host.size(), screen, page, scale * 0.70);
        QVERIFY(initial.render != resized.render);
        QTRY_COMPARE(previewImage().size(), resized.render);
        QVERIFY(previewWindow()->height() <= resized.visibleHeight + 48);
        QVERIFY(previewWindow()->width() <= screen.width() / 2);
        QVERIFY(previewWindow()->height() <= screen.height() / 2);
        QCOMPARE(calls.load(), 2);
        QVERIFY(previews.showPreview(destination(0.25)));
        QCOMPARE(calls.load(), 2); // Same render size reuses the cache.
    }

    void destinationPosition_data()
    {
        QTest::addColumn<double>("y");
        QTest::addColumn<bool>("hasY");
        QTest::addColumn<int>("top");
        QTest::newRow("page-start") << 0.0 << true << 0;
        QTest::newRow("middle") << 0.5 << true << -2;
        QTest::newRow("page-end") << 1.0 << true << -1;
        QTest::newRow("clamped") << 2.0 << true << -1;
        QTest::newRow("unspecified") << 0.5 << false << 0;
        QTest::newRow("non-finite") << std::numeric_limits<double>::quiet_NaN() << true << 0;
    }

    void destinationPosition()
    {
        QFETCH(double, y);
        QFETCH(bool, hasY);
        QFETCH(int, top);
        std::atomic_int calls = 0;
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto&) {
                ++calls;
                return solidPage(size);
            },
            singlePage(QSizeF(560, 840)));
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(y);
        if (!hasY)
            viewport.coordinateMask = 0;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        QCOMPARE(previewImage().size(), renderSize());
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::red));
        auto* scroll = previewScrollArea();
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QCOMPARE(scroll->verticalScrollBar()->value(),
                 top == -1       ? scroll->verticalScrollBar()->maximum()
                     : top == -2 ? std::min(previewImage().height() / 2 - 24, scroll->verticalScrollBar()->maximum())
                                 : top);
        // References on the same page reuse its render, regardless of destination.
        QVERIFY(previews.showPreview(destination(0.25)));
        QCOMPARE(calls.load(), 1);
    }

    void previewsNeighborPages_data()
    {
        QTest::addColumn<int>("target");
        QTest::addColumn<int>("pageCount");
        QTest::addColumn<QList<int>>("expectedPages");
        QTest::newRow("one-page") << 0 << 1 << QList<int> { 0 };
        QTest::newRow("first-page") << 0 << 3 << QList<int> { 0, 1 };
        QTest::newRow("middle-page") << 1 << 3 << QList<int> { 0, 1, 2 };
        QTest::newRow("last-page") << 2 << 3 << QList<int> { 1, 2 };
    }

    void previewsNeighborPages()
    {
        QFETCH(int, target);
        QFETCH(int, pageCount);
        QFETCH(QList<int>, expectedPages);
        QList<int> renderedPages;
        const QColor colors[] { Qt::red, Qt::green, Qt::blue };
        Generator::LinkPreview previews(
            [&](int page, QSize size, const auto&) {
                renderedPages.append(page);
                return solidPage(size, colors[page]);
            },
            [pageCount](int page) {
                return LinkPreviewPage { page >= 0 && page < pageCount ? QSizeF(640, 960 + page * 160) : QSizeF { } };
            });
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.25);
        viewport.page = target;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        QCOMPARE(renderedPages, QList<int> { target }); // Destination first; offscreen neighbors are placeholders.
        // The ready image arrives before Qt relayouts the shorter header text.
        const auto* header = previewWindow()->findChild<QLabel*>()->parentWidget();
        QTRY_COMPARE(header->height(), header->heightForWidth(header->width()));
        auto* scroll = previewScrollArea();
        auto* bar = scroll->verticalScrollBar();
        int top = 0;
        for (int page : expectedPages) {
            auto* label = previewPage(page);
            QVERIFY(label);
            QCOMPARE(label->geometry(), QRect(QPoint(0, top), renderSize(QSizeF(640, 960 + page * 160))));
            if (page != target)
                QVERIFY(pageImage(page).isNull());
            top += label->height() + 8;
        }
        QCOMPARE(scroll->widget()->size(), QSize(renderSize().width(), top - 8));
        QCOMPARE(bar->value(),
                 std::clamp(previewPage(target)->y() + previewPage(target)->height() / 4 - 24, 0, bar->maximum()));
        const int maximum = bar->maximum();
        for (int page : expectedPages) {
            const int position = scrollToPage(page);
            QTRY_COMPARE(pageImage(page).size(), previewPage(page)->size());
            QCOMPARE(pageImage(page).pixelColor(100, 100), colors[page]);
            QCOMPARE(bar->value(), position); // Loading a page must not move the viewport.
            QCOMPARE(bar->maximum(), maximum);
        }
        std::sort(renderedPages.begin(), renderedPages.end());
        QCOMPARE(renderedPages, expectedPages);
        QVERIFY(previews.showPreview(viewport));
        QCOMPARE(renderedPages, expectedPages); // Already displayed pages need no new render.
    }

    void handlesPageRenderFailure_data()
    {
        QTest::addColumn<int>("failedPage");
        QTest::newRow("previous-unavailable") << 0;
        QTest::newRow("destination-unavailable") << 1;
        QTest::newRow("next-wrong-size") << 2;
    }

    void handlesPageRenderFailure()
    {
        QFETCH(int, failedPage);
        std::atomic_bool failed = false;
        Generator::LinkPreview previews(
            [&](int page, QSize size, const auto&) {
                if (page == failedPage && !failed.exchange(true))
                    return page == 2 ? solidPage(QSize(1, 1)) : QImage { };
                return solidPage(size, page == 1 ? Qt::green : Qt::red);
            },
            pageRange(3));
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.5);
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        if (failedPage == 1) {
            auto* status = previewWindow()->findChild<QLabel*>();
            QTRY_COMPARE(status->text(), QStringLiteral("Preview unavailable"));
            QVERIFY(previewImage().isNull());
        } else {
            QTRY_COMPARE(previewImage().size(), renderSize());
            const auto* header = previewWindow()->findChild<QLabel*>()->parentWidget();
            QTRY_COMPARE(header->height(), header->heightForWidth(header->width()));
            auto* scroll = previewScrollArea();
            auto* label = previewPage(failedPage);
            const int maximum = scroll->verticalScrollBar()->maximum();
            const int position = scrollToPage(failedPage);
            QTRY_COMPARE(label->text(), QStringLiteral("Preview unavailable"));
            QCOMPARE(label->size(), renderSize()); // Keep the reserved space, even on failure.
            QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::green));
            QCOMPARE(scroll->verticalScrollBar()->value(), position);
            QCOMPARE(scroll->verticalScrollBar()->maximum(), maximum);
        }
        QTest::keyClick(&m_host, Qt::Key_Escape);
        QTRY_VERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        scrollToPage(failedPage);
        QTRY_COMPARE(pageImage(failedPage).size(), renderSize()); // A later hover can retry a transient failure.
    }

    void dismissCancelsLazyRender_data()
    {
        QTest::addColumn<int>("blockedPage");
        QTest::newRow("previous-page") << 0;
        QTest::newRow("destination-page") << 1;
        QTest::newRow("next-page") << 2;
    }

    void dismissCancelsLazyRender()
    {
        QFETCH(int, blockedPage);
        std::atomic_bool blocked = false;
        std::atomic_bool cancelled = false;
        std::atomic_int calls = 0;
        Generator::LinkPreview previews(
            [&](int page, QSize size, const auto& isCancelled) {
                ++calls;
                if (page == blockedPage && !blocked.exchange(true)) {
                    while (!isCancelled())
                        QThread::msleep(1);
                    cancelled = true;
                }
                return solidPage(size, Qt::blue);
            },
            pageRange(3));
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.25);
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        if (blockedPage != 1) {
            QTRY_VERIFY(!previewImage().isNull());
            scrollToPage(blockedPage);
        }
        QTRY_VERIFY(blocked.load());
        QTest::keyClick(&m_host, Qt::Key_Escape);
        QTRY_VERIFY(cancelled.load());
        QVERIFY(!previewWindow());
        const int before = blockedPage == 1 ? 1 : 2;
        QCOMPARE(calls.load(), before);
        QTRY_VERIFY(previews.showPreview(viewport));
        QTRY_COMPARE(previewImage().size(), renderSize());
        if (blockedPage != 1) {
            scrollToPage(blockedPage);
        }
        QTRY_VERIFY(!pageImage(blockedPage).isNull());
        QCOMPARE(pageImage(blockedPage).pixelColor(100, 100), QColor(Qt::blue));
        QCOMPARE(calls.load(), before + 1); // Cancellation is retryable; no hidden render queue remains.
    }

    void rendersLatestVisibleNeighbor()
    {
        std::atomic_bool targetStarted = false;
        std::atomic_bool releaseTarget = false;
        QList<int> renderedPages;
        Generator::LinkPreview previews(
            [&](int page, QSize size, const auto& cancelled) {
                renderedPages.append(page);
                if (page == 1) {
                    targetStarted = true;
                    while (!releaseTarget.load() && !cancelled())
                        QThread::msleep(1);
                }
                return solidPage(size);
            },
            pageRange(3));
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.5);
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(targetStarted.load());
        auto* bar = previewScrollArea()->verticalScrollBar();
        bar->setValue(0);
        QCoreApplication::processEvents();
        bar->setValue(bar->maximum());
        QCoreApplication::processEvents();
        releaseTarget = true;
        QTRY_VERIFY(!pageImage(2).isNull());
        QCOMPARE(renderedPages, (QList<int> { 1, 2 }));
        QVERIFY(pageImage(0).isNull()); // Passing a placeholder never queues obsolete work.
    }

    void reusesPagesAcrossDestinations()
    {
        std::atomic_int calls = 0;
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto&) {
                ++calls;
                return solidPage(size);
            },
            pageRange(3));
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.25);
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        viewport.page = 2;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        QCOMPARE(calls.load(), 2);
        auto* bar = previewScrollArea()->verticalScrollBar();
        bar->setValue(0);
        QTRY_VERIFY(!pageImage(1).isNull());
        QCOMPARE(calls.load(), 2); // The old destination is now a cached neighbor.
    }

    void loadingNeighborPreservesMomentum()
    {
        std::atomic_bool neighborStarted = false;
        std::atomic_bool releaseNeighbor = false;
        Generator::LinkPreview previews(
            [&](int page, QSize size, const auto& cancelled) {
                if (page == 2) {
                    neighborStarted = true;
                    while (!releaseNeighbor.load() && !cancelled())
                        QThread::msleep(1);
                }
                return solidPage(size);
            },
            pageRange(3));
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.25);
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* scroll = previewScrollArea();
        QTest::mouseMove(scroll->viewport(), scroll->viewport()->rect().center());
        auto* bar = scroll->verticalScrollBar();
        auto* scroller = QScroller::scroller(scroll->viewport());
        const int maximum = bar->maximum();
        // Use a long Qt scroll animation so a slow worker/test host still has
        // time to deliver the neighbor while the viewport is in motion.
        scroller->scrollTo(QPointF(0, maximum), 2000);
        QTRY_VERIFY(neighborStarted.load());
        QCOMPARE(scroller->state(), QScroller::Scrolling);
        releaseNeighbor = true;
        QTRY_VERIFY(!pageImage(2).isNull());
        QCOMPARE(scroller->state(), QScroller::Scrolling);
        QCOMPARE(bar->maximum(), maximum);
        QTRY_COMPARE(scroller->state(), QScroller::Inactive);
        QCOMPARE(bar->value(), maximum);
        QVERIFY(window->isVisible());
    }

    void staysOpenAndScrollsUnderPointer()
    {
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
        auto* scroll = previewScrollArea();
        QCOMPARE(scroll->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(scroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QVERIFY(!scroll->verticalScrollBar()->isVisible());
        QVERIFY(!scroll->horizontalScrollBar()->isVisible());
        QCOMPARE(scroll->viewport()->width(), previewImage().width());
        // Cross the gap between the source link and the preview within its grace period.
        QTest::mouseMove(&m_host, QPoint(45, 45));
        QTest::mouseMove(scroll->viewport(), scroll->viewport()->rect().center());
        auto* underPointer = QApplication::widgetAt(QCursor::pos());
        QVERIFY(underPointer && (underPointer == window || window->isAncestorOf(underPointer)));
        QTest::qWait(450);
        QVERIFY(window->isVisible());
        sendWheel(scroll->viewport(), -120);
        QTRY_VERIFY(scroll->verticalScrollBar()->value() > 0);
        QCOMPARE(m_host.wheels, 0);
        QVERIFY(window->isVisible());
        // Real wheel input first passes through the native preview window.
        const int previous = scroll->verticalScrollBar()->value();
        const QPoint position = window->mapFromGlobal(QCursor::pos());
        QTest::wheelEvent(window->windowHandle(), position, QPoint(0, -120));
        QVERIFY(window->isVisible());
        QTRY_VERIFY(scroll->verticalScrollBar()->value() > previous);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        sendWheel(scroll->viewport(), -120);
        QCOMPARE(m_host.wheels, 0); // Reaching the page edge must not scroll the document.
        QVERIFY(window->isVisible());
        QTest::mouseMove(&m_host, QPoint(5, 5));
        QTRY_VERIFY(!window->isVisible());
    }

    void pinFitsHeader_data()
    {
        QTest::addColumn<int>("fontSize");
        QTest::newRow("small-font") << 8;
        QTest::newRow("normal-font") << 11;
        QTest::newRow("large-font") << 18;
    }

    void pinFitsHeader()
    {
        QFETCH(int, fontSize);
        const QFont original = QApplication::font();
        const auto restoreFont = qScopeGuard([&] { QApplication::setFont(original); });
        QFont font = original;
        font.setPointSize(fontSize);
        QApplication::setFont(font);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 840)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        const auto* pin = window->findChild<QToolButton*>(QStringLiteral("previewPin"));
        const auto* header = window->findChild<QWidget*>(QStringLiteral("previewHeader"));
        QVERIFY(pin);
        QVERIFY(header);
        QVERIFY(pin->height() <= header->fontMetrics().height());
        QVERIFY(pin->iconSize().height() < pin->height());
        QVERIFY(header->rect().contains(pin->geometry()));
    }

    void pinsPreview_data()
    {
        QTest::addColumn<bool>("unpin");
        QTest::addColumn<bool>("scrollDocument");
        QTest::newRow("stay-open") << false << false;
        QTest::newRow("unpin") << true << false;
        QTest::newRow("document-scroll") << false << true;
        QTest::newRow("unpin-after-document-scroll") << true << true;
    }

    void pinsPreview()
    {
        QFETCH(bool, unpin);
        QFETCH(bool, scrollDocument);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* pin = window->findChild<QToolButton*>(QStringLiteral("previewPin"));
        QVERIFY(pin);
        QVERIFY(!pin->isChecked());
        const QPoint initial = window->pos();
        QTest::mouseClick(pin, Qt::LeftButton);
        QVERIFY(pin->isChecked());
        QCOMPARE(window->pos(), initial); // The button must not start a header drag.
        // Repeated tooltip queries for the same source must preserve the pin.
        QTest::mouseMove(&m_host, QPoint(40, 40));
        QVERIFY(previews.showPreview(destination()));
        QVERIFY(pin->isChecked());
        QTest::mouseMove(&m_host, QPoint(5, 5));
        if (scrollDocument) {
            sendWheel(&m_host, -120);
            QTest::wheelEvent(m_host.windowHandle(), QPoint(5, 5), QPoint(0, -120));
            QCOMPARE(m_host.wheels, 2); // Pinning must not consume document input.
        }
        QTest::qWait(450);
        QVERIFY(window->isVisible());
        QVERIFY(pin->isChecked());
        if (unpin) {
            QTest::mouseClick(pin, Qt::LeftButton);
            QVERIFY(!pin->isChecked());
            QTest::mouseMove(&m_host, QPoint(5, 5));
            QTRY_VERIFY(!window->isVisible());
        }
    }

    void anotherLinkClearsPin_data()
    {
        QTest::addColumn<bool>("sameDestination");
        QTest::newRow("same-destination") << true;
        QTest::newRow("different-destination") << false;
    }

    void anotherLinkClearsPin()
    {
        QFETCH(bool, sameDestination);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); }, pageRange(2));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* pin = window->findChild<QToolButton*>(QStringLiteral("previewPin"));
        QTest::mouseClick(pin, Qt::LeftButton);
        QTest::mouseMove(&m_host, QPoint(40, 45));
        auto next = destination();
        next.page = sameDestination ? 0 : 1;
        QVERIFY(previews.showPreview(next));
        QTRY_VERIFY(!previewImage().isNull());
        QVERIFY(!pin->isChecked());
        QTest::mouseMove(&m_host, QPoint(5, 5));
        QTRY_VERIFY(!window->isVisible());
    }

    void dragsHeader_data()
    {
        QTest::addColumn<Qt::MouseButton>("button");
        QTest::addColumn<bool>("hint");
        QTest::newRow("page-label") << Qt::LeftButton << false;
        QTest::newRow("hint-label") << Qt::LeftButton << true;
        QTest::newRow("right-button") << Qt::RightButton << false;
        QTest::newRow("middle-button") << Qt::MiddleButton << false;
    }

    void dragsHeader()
    {
        QFETCH(Qt::MouseButton, button);
        QFETCH(bool, hint);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* header = window->findChild<QWidget*>(QStringLiteral("previewHeader"));
        QVERIFY(header);
        const auto labels = header->findChildren<QLabel*>();
        QCOMPARE(labels.size(), 2);
        const auto* label = labels.at(hint ? 1 : 0);
        const QPoint start = window->mapFromGlobal(label->mapToGlobal(label->rect().center()));
        const QPoint initial = window->pos();
        const QPoint delta(45, 35);
        const int scrollPosition = previewScrollArea()->verticalScrollBar()->value();
        QTest::mousePress(window->windowHandle(), button, Qt::NoModifier, start);
        const auto releaseMouse = qScopeGuard([&] { QTest::mouseRelease(m_host.windowHandle(), button); });
        QCOMPARE(header->cursor().shape(), button == Qt::LeftButton ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
        QTest::mouseMove(window->windowHandle(), start + delta);
        QCOMPARE(window->pos(), button == Qt::LeftButton ? initial + delta : initial);
        QCOMPARE(previewScrollArea()->verticalScrollBar()->value(), scrollPosition);
        const QPoint release = window->mapFromGlobal(initial + start + delta);
        QTest::mouseRelease(window->windowHandle(), button, Qt::NoModifier, release);
        QCOMPARE(header->cursor().shape(), Qt::OpenHandCursor);
        const QPoint moved = window->pos();
        QTest::mouseMove(window->windowHandle(), release + QPoint(10, 0));
        QCOMPARE(window->pos(), moved);
    }

    void dragsPage_data()
    {
        QTest::addColumn<Qt::MouseButton>("button");
        QTest::addColumn<int>("distance");
        QTest::addColumn<int>("scrollDirection");
        QTest::newRow("left-up") << Qt::LeftButton << -80 << 1;
        QTest::newRow("left-down") << Qt::LeftButton << 80 << -1;
        QTest::newRow("right-ignored") << Qt::RightButton << -80 << 0;
        QTest::newRow("middle-ignored") << Qt::MiddleButton << -80 << 0;
    }

    void dragsPage()
    {
        QFETCH(Qt::MouseButton, button);
        QFETCH(int, distance);
        QFETCH(int, scrollDirection);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* scroll = previewScrollArea();
        const int initial = scroll->verticalScrollBar()->value();
        const QPoint start =
            window->mapFromGlobal(scroll->viewport()->mapToGlobal(scroll->viewport()->rect().center()));
        QTest::mouseMove(window->windowHandle(), start);
        QTest::mousePress(window->windowHandle(), button, Qt::NoModifier, start);
        bool released = false;
        const auto releaseMouse = qScopeGuard([&] {
            if (!released)
                QTest::mouseRelease(m_host.windowHandle(), button);
        });
        if (button == Qt::LeftButton)
            QCOMPARE(scroll->viewport()->cursor().shape(), Qt::ClosedHandCursor);
        QTest::mouseMove(window->windowHandle(), start + QPoint(0, distance));
        // Qt applies the platform drag threshold and updates on animation frames.
        if (scrollDirection > 0)
            QTRY_VERIFY(scroll->verticalScrollBar()->value() > initial);
        else if (scrollDirection < 0)
            QTRY_VERIFY(scroll->verticalScrollBar()->value() < initial);
        else
            QCOMPARE(scroll->verticalScrollBar()->value(), initial);
        QTest::mouseRelease(window->windowHandle(), button, Qt::NoModifier, start + QPoint(0, distance));
        released = true;
        QCOMPARE(scroll->viewport()->cursor().shape(), Qt::OpenHandCursor);
        QTest::mouseMove(window->windowHandle(), start);
        if (button != Qt::LeftButton)
            QCOMPARE(scroll->verticalScrollBar()->value(), initial);
        QCOMPARE(m_host.clicks, 0);
        QVERIFY(window->isVisible());
    }

    void flicksPage_data()
    {
        QTest::addColumn<int>("direction");
        QTest::newRow("up") << -1;
        QTest::newRow("down") << 1;
    }

    void flicksPage()
    {
        QFETCH(int, direction);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* scroll = previewScrollArea();
        auto* bar = scroll->verticalScrollBar();
        auto* scroller = QScroller::scroller(scroll->viewport());
        const QPoint viewportPosition = scroll->viewport()->pos();
        flickPreview(window, direction);
        QCOMPARE(scroller->state(), QScroller::Scrolling);
        QCOMPARE(scroll->viewport()->cursor().shape(), Qt::OpenHandCursor);
        const int releasedAt = bar->value();
        QTRY_VERIFY(direction < 0 ? bar->value() > releasedAt : bar->value() < releasedAt);
        // Relinquishing the implicit grab after a normal release must not kill the flick.
        QEvent ungrab(QEvent::UngrabMouse);
        QCoreApplication::sendEvent(scroll->viewport(), &ungrab);
        QCOMPARE(scroller->state(), QScroller::Scrolling);
        QTRY_COMPARE(scroller->state(), QScroller::Inactive);
        QVERIFY(bar->value() >= 0 && bar->value() <= bar->maximum());
        QCOMPARE(scroll->viewport()->pos(), viewportPosition); // No edge overshoot/blank strips.
        QVERIFY(window->isVisible());
        QCOMPARE(m_host.clicks, 0);
        QCOMPARE(m_host.wheels, 0);
    }

    void interruptsFlick_data()
    {
        QTest::addColumn<int>("activity");
        QTest::newRow("press") << 0;
        QTest::newRow("wheel") << 1;
        QTest::newRow("escape") << 2;
        QTest::newRow("owner-hidden") << 3;
        QTest::newRow("new-destination") << 4;
        QTest::newRow("disabled") << 5;
        QTest::newRow("reset") << 6;
        QTest::newRow("pointer-leaves") << 7;
    }

    void interruptsFlick()
    {
        QFETCH(int, activity);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
        auto* scroll = previewScrollArea();
        QPointer<QScroller> scroller = QScroller::scroller(scroll->viewport());
        flickPreview(window, -1);
        QCOMPARE(scroller->state(), QScroller::Scrolling);
        if (activity == 0) {
            const QPoint start =
                window->mapFromGlobal(scroll->viewport()->mapToGlobal(scroll->viewport()->rect().center()));
            QTest::mousePress(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, start);
            const auto releaseMouse = qScopeGuard(
                [&] { QTest::mouseRelease(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, start); });
            QCOMPARE(scroller->state(), QScroller::Pressed);
            const int stoppedAt = scroll->verticalScrollBar()->value();
            QTest::qWait(80);
            QCOMPARE(scroll->verticalScrollBar()->value(), stoppedAt);
        } else if (activity == 1) {
            sendWheel(scroll->viewport(), -120);
            QCOMPARE(m_host.wheels, 0);
        } else if (activity == 2) {
            QTest::keyClick(&m_host, Qt::Key_Escape);
        } else if (activity == 3) {
            m_host.hide();
        } else if (activity == 4) {
            QTest::mouseMove(&m_host, QPoint(40, 40));
            QVERIFY(previews.showPreview(destination(0.25)));
        } else if (activity == 5) {
            previews.setMode(LinkPreviewMode::Off);
        } else if (activity == 6) {
            previews.reset();
        } else {
            QTest::mouseMove(&m_host, QPoint(5, 5));
            QTRY_VERIFY(!window->isVisible());
        }
        QVERIFY(!scroller || scroller->state() == QScroller::Inactive);
        if (activity == 0 || activity == 1 || activity == 4) {
            const int stoppedAt = scroll->verticalScrollBar()->value();
            QTest::qWait(80);
            QCOMPARE(scroll->verticalScrollBar()->value(), stoppedAt);
            QVERIFY(window->isVisible());
        } else {
            QVERIFY(!window || !window->isVisible());
        }
    }

    void dragOutsideKeepsPreviewUntilFinished_data()
    {
        QTest::addColumn<bool>("loseGrab");
        QTest::addColumn<bool>("headerDrag");
        QTest::newRow("released") << false << false;
        QTest::newRow("grab-lost") << true << false;
        QTest::newRow("header-released") << false << true;
        QTest::newRow("header-grab-lost") << true << true;
    }

    void dragOutsideKeepsPreviewUntilFinished()
    {
        QFETCH(bool, loseGrab);
        QFETCH(bool, headerDrag);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
        auto* dragTarget =
            headerDrag ? window->findChild<QWidget*>(QStringLiteral("previewHeader")) : previewScrollArea()->viewport();
        const QPoint start = window->mapFromGlobal(dragTarget->mapToGlobal(dragTarget->rect().center()));
        const QPoint outside(window->width() + 10, start.y());
        // Native QtTest mouse events don't move QCursor on the offscreen
        // platform. Set the position used by the hover check explicitly.
        QCursor::setPos(window->mapToGlobal(outside));
        QCoreApplication::processEvents();
        QVERIFY(!window->frameGeometry().contains(QCursor::pos()));
        QTest::mousePress(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, start);
        bool released = false;
        const auto releaseMouse = qScopeGuard([&] {
            if (!released)
                QTest::mouseRelease(m_host.windowHandle(), Qt::LeftButton);
        });
        // Moving the header would move the window under the cursor. Hold
        // that gesture here to exercise the timer with an outside cursor.
        if (!headerDrag)
            QTest::mouseMove(window->windowHandle(), outside);
        QTest::qWait(450);
        QVERIFY(window->isVisible());
        if (loseGrab) {
            QEvent event(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(dragTarget, &event);
        } else {
            QTest::mouseRelease(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, outside);
            released = true;
        }
        // Qt may synthesize a final move before releasing the header grab.
        // Once the gesture ends, leaving the moved frame must dismiss it.
        if (headerDrag)
            QCursor::setPos(window->mapToGlobal(outside));
        QTRY_VERIFY(!window->isVisible());
    }

    void interruptedDrag_data()
    {
        QTest::addColumn<bool>("loseGrab");
        QTest::addColumn<bool>("headerDrag");
        QTest::newRow("dismissed") << false << false;
        QTest::newRow("grab-lost") << true << false;
        QTest::newRow("header-dismissed") << false << true;
        QTest::newRow("header-grab-lost") << true << true;
    }

    void interruptedDrag()
    {
        QFETCH(bool, loseGrab);
        QFETCH(bool, headerDrag);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* scroll = previewScrollArea();
        auto* dragTarget =
            headerDrag ? window->findChild<QWidget*>(QStringLiteral("previewHeader")) : scroll->viewport();
        const QPoint start = window->mapFromGlobal(dragTarget->mapToGlobal(dragTarget->rect().center()));
        QTest::mousePress(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, start);
        const auto releaseMouse = qScopeGuard([&] { QTest::mouseRelease(m_host.windowHandle(), Qt::LeftButton); });
        QCOMPARE(dragTarget->cursor().shape(), Qt::ClosedHandCursor);
        const int initial = scroll->verticalScrollBar()->value();
        const QPoint initialPosition = window->pos();
        if (loseGrab) {
            QEvent event(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(dragTarget, &event);
            QTest::mouseMove(window->windowHandle(), start + QPoint(0, -80));
            QCOMPARE(scroll->verticalScrollBar()->value(), initial);
            QCOMPARE(window->pos(), initialPosition);
            QVERIFY(window->isVisible());
        } else {
            QTest::keyClick(&m_host, Qt::Key_Escape);
            QVERIFY(!window->isVisible());
        }
        QCOMPARE(dragTarget->cursor().shape(), Qt::OpenHandCursor);
    }

    void dismissesOnInteraction_data()
    {
        QTest::addColumn<int>("activity");
        QTest::addColumn<bool>("pinned");
        QTest::newRow("escape") << 0 << false;
        QTest::newRow("outside-click") << 1 << false;
        QTest::newRow("source-scroll") << 2 << false;
        QTest::newRow("source-hidden") << 3 << false;
        QTest::newRow("window-deactivated") << 4 << false;
        QTest::newRow("document-reset") << 5 << false;
        QTest::newRow("pinned-escape") << 0 << true;
        QTest::newRow("pinned-outside-click") << 1 << true;
        QTest::newRow("pinned-source-hidden") << 3 << true;
        QTest::newRow("pinned-window-deactivated") << 4 << true;
        QTest::newRow("pinned-document-reset") << 5 << true;
    }

    void dismissesOnInteraction()
    {
        QFETCH(int, activity);
        QFETCH(bool, pinned);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 840)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
        if (pinned)
            QTest::mouseClick(window->findChild<QToolButton*>(QStringLiteral("previewPin")), Qt::LeftButton);
        if (activity == 0) {
            QTest::keyClick(&m_host, Qt::Key_Escape);
        } else if (activity == 1) {
            QTest::mouseClick(&m_host, Qt::LeftButton, Qt::NoModifier, QPoint(40, 40));
            QCOMPARE(m_host.clicks, 1); // Dismissal must not consume the original link click.
        } else if (activity == 2) {
            sendWheel(&m_host, -120);
            QCOMPARE(m_host.wheels, 1);
        } else if (activity == 3) {
            m_host.hide();
        } else if (activity == 4) {
            QEvent deactivate(QEvent::WindowDeactivate);
            QApplication::sendEvent(&m_host, &deactivate);
        } else {
            previews.reset();
        }
        QTRY_VERIFY(!window || !window->isVisible());
        if (window)
            QVERIFY(!window->findChild<QToolButton*>(QStringLiteral("previewPin"))->isChecked());
    }

    void resetCancelsAndDiscardsOldCompletion()
    {
        std::atomic_int calls = 0;
        std::atomic_bool started = false;
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto& cancelled) {
                const int call = ++calls;
                started = true;
                if (call == 1)
                    while (!cancelled())
                        QThread::msleep(1);
                return solidPage(size, call == 1 ? Qt::red : Qt::blue);
            },
            singlePage(QSizeF(560, 840)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(started.load());
        previews.reset();
        QVERIFY(!previewWindow());
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::blue));
        QCOMPARE(calls.load(), 2);
    }

    void resetDiscardsNeighborCompletion_data()
    {
        QTest::addColumn<int>("neighbor");
        QTest::newRow("previous") << 0;
        QTest::newRow("next") << 2;
    }

    void resetDiscardsNeighborCompletion()
    {
        QFETCH(int, neighbor);
        std::atomic_bool started = false;
        std::atomic_bool cancelled = false;
        bool reopened = false;
        Generator::LinkPreview previews(
            [&](int page, QSize size, const auto& isCancelled) {
                if (page == neighbor) {
                    started = true;
                    while (!isCancelled())
                        QThread::msleep(1);
                    cancelled = true;
                    return solidPage(size, Qt::red);
                }
                return solidPage(size, Qt::blue);
            },
            [&](int page) {
                const bool available = reopened ? page == 1 : page >= 0 && page < 3;
                return LinkPreviewPage { available ? QSizeF(640, 960) : QSizeF { } };
            });
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.5);
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        scrollToPage(neighbor);
        QTRY_VERIFY(started.load());
        previews.reset();
        QVERIFY(cancelled.load());
        reopened = true; // The old neighbor index now belongs to a different/smaller layout.
        QVERIFY(previews.showPreview(viewport));
        QTRY_COMPARE(previewImage().size(), renderSize());
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::blue));
        QVERIFY(!previewPage(neighbor));
        QCOMPARE(previewPage(1)->y(), 0);
    }

    void sourceWindowDestroyedDuringRender()
    {
        auto host = std::make_unique<QWidget>();
        host->setGeometry(40, 40, 700, 500);
        host->show();
        QVERIFY(QTest::qWaitForWindowExposed(host.get()));
        QTest::mouseMove(host.get(), QPoint(40, 40));
        std::atomic_int calls = 0;
        std::atomic_bool started = false;
        std::atomic_bool cancelled = false;
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto& isCancelled) {
                if (++calls == 1) {
                    started = true;
                    while (!isCancelled())
                        QThread::msleep(1);
                    cancelled = true;
                    return solidPage(size, Qt::red);
                }
                return solidPage(size, Qt::blue);
            },
            singlePage(QSizeF(640, 960)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(started.load());
        QPointer<QWidget> window = previewWindow();
        host.reset();
        QVERIFY(window.isNull());
        QTRY_VERIFY(cancelled.load());
        QTest::mouseMove(&m_host, QPoint(40, 40));
        QTRY_VERIFY(previews.showPreview(destination()));
        QTRY_COMPARE(previewImage().size(), renderSize());
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::blue));
        QCOMPARE(calls.load(), 2);
    }

    void cacheReleasesWorkerFrame()
    {
        const auto released = std::make_shared<std::atomic_bool>(true);
        std::atomic_int calls = 0;
        std::atomic_bool overlappingLeases = false;
        using Frame = std::pair<QImage, std::shared_ptr<std::atomic_bool>>;
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto&) {
                ++calls;
                if (!released->exchange(false))
                    overlappingLeases = true;
                auto* frame = new Frame(solidPage(size), released);
                return QImage(
                    frame->first.constBits(),
                    size.width(),
                    size.height(),
                    frame->first.bytesPerLine(),
                    frame->first.format(),
                    [](void* context) {
                        auto* owner = static_cast<Frame*>(context);
                        owner->second->store(true);
                        delete owner;
                    },
                    frame);
            },
            pageRange(3));
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.5);
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        for (int page : { 0, 2 }) {
            scrollToPage(page);
            QTRY_VERIFY(!pageImage(page).isNull());
        }
        // All pages remain cached/displayed, but every transport lease is free.
        QVERIFY(released->load());
        QVERIFY(!overlappingLeases.load());
        QCOMPARE(calls.load(), 3);
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::red));
    }

    void sourceWindowOwnsPreview()
    {
        auto host = std::make_unique<QWidget>();
        host->setGeometry(40, 40, 700, 500);
        host->show();
        QVERIFY(QTest::qWaitForWindowExposed(host.get()));
        QTest::mouseMove(host.get(), QPoint(40, 40));
        QTRY_COMPARE(QApplication::widgetAt(QCursor::pos()), host.get());
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 840)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
        host.reset();
        QVERIFY(window.isNull());
        previews.reset(); // Qt may destroy the window before the generator.
    }

    void fitsScreenWithoutCoveringLink()
    {
        QTest::mouseMove(&m_host, QPoint(m_host.width() - 5, m_host.height() - 5));
        const QPoint anchor = QCursor::pos();
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 840)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        const auto* window = previewWindow();
        QVERIFY(m_host.screen()->availableGeometry().contains(window->frameGeometry()));
        QVERIFY(!window->frameGeometry().contains(anchor));
    }

    void boundsUnusualGeometry_data()
    {
        QTest::addColumn<QSizeF>("geometry");
        QTest::newRow("wide") << QSizeF(1120, 560);
        QTest::newRow("extremely-wide") << QSizeF(1e300, 1e-300);
        QTest::newRow("extremely-tall") << QSizeF(1e-300, 1e300);
    }

    void boundsUnusualGeometry()
    {
        QFETCH(QSizeF, geometry);
        const QSize expected = renderSize(geometry);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(geometry));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QCOMPARE(previewImage().size(), expected);
    }

    void linksKeepNavigationAndExternalActions()
    {
        int pageQueries = 0;
        bool available = false;
        auto previews = std::make_unique<Generator::LinkPreview>(
            [](int, QSize size, const auto&) { return solidPage(size); },
            [&](int page) {
                ++pageQueries;
                return LinkPreviewPage { available && page == 0 ? QSizeF(560, 840) : QSizeF { } };
            });
        Model::Link internal;
        internal.target.valid = true;
        internal.target.viewport = destination(0.5);
        Model::Link external = internal;
        external.target.external = true;
        external.target.uri = "https://example.com/";
        auto rects = Generator::Conversion::objectRects({ internal, external }, previews.get());
        const auto cleanup = qScopeGuard([&] { qDeleteAll(rects); });
        QCOMPARE(rects.size(), 2);
        QCOMPARE(pageQueries, 0);
        const auto* go =
            dynamic_cast<const Okular::GotoAction*>(static_cast<const Okular::Action*>(rects[0]->object()));
        QVERIFY(go);
        QCOMPARE(go->destViewport().pageNumber, 0);
        QCOMPARE(go->destViewport().rePos.normalizedY, 0.5);
        const auto* browse = static_cast<const Okular::Action*>(rects[1]->object());
        QCOMPARE(browse->actionType(), Okular::Action::Browse);
        browse->actionTip();
        QCOMPARE(pageQueries, 0);
        const QString fallback = go->actionTip();
        QCOMPARE(pageQueries, 0); // Off keeps the native tooltip without page/render work.
        previews->setMode(LinkPreviewMode::Large);
        QCOMPARE(go->actionTip(), fallback);
        QCOMPARE(pageQueries, 1);
        available = true;
        QVERIFY(go->actionTip().isEmpty());
        QTRY_VERIFY(!previewImage().isNull());
        previews.reset();
        QVERIFY(!previewWindow());
        QCOMPARE(go->actionTip(), fallback);
    }

    void rendersThroughWorker()
    {
        Plugin::WorkerClient worker;
        QVERIFY(worker.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(worker.open(QStringLiteral(TEST_PDF_PATH), { }, pages), Model::OpenStatus::Success);
        QVERIFY(!pages.isEmpty());
        Generator::LinkPreview previews(
            [&](int page, QSize size, const auto& cancelled) {
                return worker.render(page, size.width(), size.height(), { }, cancelled);
            },
            [&](int page) {
                if (page < 0 || page >= pages.size())
                    return LinkPreviewPage { };
                const auto& geometry = pages.at(page).geometry;
                return LinkPreviewPage { QSizeF(geometry.widthPoints, geometry.heightPoints) };
            });
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY_WITH_TIMEOUT(!previewImage().isNull(), 10000);
        QVERIFY(previewWindow()->width() <= m_host.screen()->geometry().width() / 3);
        QVERIFY(previewImage().height() <= 4096);
    }
};

QTEST_MAIN(TestLinkPreview)
#include "test_link_preview.moc"
