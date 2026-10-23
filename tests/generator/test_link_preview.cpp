// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/document.hpp"
#include "generator/link_preview.hpp"
#include "plugin/worker_client.hpp"

#include <QApplication>
#include <QCursor>
#include <QLabel>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <QtTest/qtestwheel.h>
#include <okular/core/page.h>

#include <atomic>
#include <limits>
#include <memory>
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

auto singlePage(QSizeF size)
{
    return [size](int page) {
        return LinkPreviewPage { page == 0 ? size : QSizeF { } };
    };
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

QImage previewImage()
{
    const auto* window = previewWindow();
    const auto* label = window ? window->findChild<QLabel*>(QStringLiteral("previewImage")) : nullptr;
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
        QTest::addColumn<double>("font");
        QTest::addColumn<QSize>("render");
        QTest::addColumn<int>("height");
        const QSize window(1000, 800), screen(1920, 1080);
        const QSizeF page(600, 900);
        QTest::newRow("off") << LinkPreviewMode::Off << window << screen << page << 0.0 << QSize() << 0;
        QTest::newRow("small") << LinkPreviewMode::Small << window << screen << page << 0.0 << QSize(480, 720) << 300;
        QTest::newRow("large") << LinkPreviewMode::Large << window << screen << page << 0.0 << QSize(640, 960) << 400;
        QTest::newRow("auto-pdf") << LinkPreviewMode::AutoSize << window << screen << page << 0.0 << QSize(550, 825)
                                  << 440;
        QTest::newRow("auto-small-font") << LinkPreviewMode::AutoSize << window << screen << page << 10.0
                                         << QSize(700, 1050) << 440;
        QTest::newRow("auto-large-font") << LinkPreviewMode::AutoSize << window << screen << page << 20.0
                                         << QSize(480, 720) << 440;
        QTest::newRow("auto-large-window") << LinkPreviewMode::AutoSize << QSize(2400, 1600) << QSize(2560, 1920)
                                           << page << 0.0 << QSize(800, 1200) << 560;
        QTest::newRow("screen-bound") << LinkPreviewMode::Large << window << QSize(320, 240) << page << 0.0
                                      << QSize(272, 408) << 192;
        QTest::newRow("short-page") << LinkPreviewMode::Large << window << screen << QSizeF(1200, 300) << 0.0
                                    << QSize(640, 160) << 160;
        QTest::newRow("invalid-page") << LinkPreviewMode::Large << window << screen << QSizeF(0, 900) << 0.0 << QSize()
                                      << 0;
        QTest::newRow("non-finite-page") << LinkPreviewMode::AutoSize << window << screen
                                         << QSizeF(std::numeric_limits<double>::infinity(), 900) << 11.0 << QSize()
                                         << 0;
        QTest::newRow("invalid-font") << LinkPreviewMode::AutoSize << window << screen << page
                                      << std::numeric_limits<double>::quiet_NaN() << QSize(550, 825) << 440;
        QTest::newRow("no-window") << LinkPreviewMode::AutoSize << QSize() << screen << page << 11.0 << QSize() << 0;
        QTest::newRow("no-screen") << LinkPreviewMode::Large << window << QSize() << page << 0.0 << QSize() << 0;
        QTest::newRow("invalid-mode") << static_cast<LinkPreviewMode>(99) << window << screen << page << 0.0 << QSize()
                                      << 0;
    }

    void sizesPreview()
    {
        QFETCH(LinkPreviewMode, mode);
        QFETCH(QSize, window);
        QFETCH(QSize, screen);
        QFETCH(QSizeF, page);
        QFETCH(double, font);
        QFETCH(QSize, render);
        QFETCH(int, height);
        const auto size = Generator::sizeLinkPreview(mode, window, screen, { page, font });
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
        QTRY_COMPARE(previewImage().size(), QSize(480, 720));
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::blue));
        QCOMPARE(calls.load(), 2);
    }

    void autoFollowsWindowSize_data()
    {
        QTest::addColumn<double>("font");
        QTest::newRow("pdf") << 0.0;
        QTest::newRow("epub-mobi-small-font") << 10.0;
        QTest::newRow("epub-mobi-large-font") << 20.0;
    }

    void autoFollowsWindowSize()
    {
        QFETCH(double, font);
        std::atomic_int calls = 0;
        const LinkPreviewPage page { QSizeF(560, 840), font };
        Generator::LinkPreview previews(
            [&](int, QSize size, const auto&) {
                ++calls;
                return solidPage(size);
            },
            [page](int index) { return index == 0 ? page : LinkPreviewPage { }; });
        previews.setMode(LinkPreviewMode::AutoSize);
        const auto screen = m_host.screen()->availableGeometry().size();
        QVERIFY(previews.showPreview(destination()));
        const auto initial = Generator::sizeLinkPreview(LinkPreviewMode::AutoSize, m_host.size(), screen, page);
        QTRY_COMPARE(previewImage().size(), initial.render);
        QVERIFY(previewWindow()->height() <= initial.visibleHeight + 48);
        m_host.resize(600, 460);
        QTest::mouseMove(&m_host, QPoint(40, 40));
        QVERIFY(previews.showPreview(destination()));
        const auto resized = Generator::sizeLinkPreview(LinkPreviewMode::AutoSize, m_host.size(), screen, page);
        QVERIFY(initial.render != resized.render);
        QTRY_COMPARE(previewImage().size(), resized.render);
        QVERIFY(previewWindow()->height() <= resized.visibleHeight + 48);
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
        QTest::newRow("middle") << 0.5 << true << 456;
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
        QCOMPARE(previewImage().size(), QSize(640, 960));
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::red));
        auto* scroll = previewWindow()->findChild<QScrollArea*>();
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QCOMPARE(scroll->verticalScrollBar()->value(), top < 0 ? scroll->verticalScrollBar()->maximum() : top);
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
                return LinkPreviewPage { page >= 0 && page < pageCount ? QSizeF(640, 480 + page * 160) : QSizeF { } };
            });
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination(0.5);
        viewport.page = target;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        QCOMPARE(renderedPages, expectedPages);
        const QImage strip = previewImage();
        int top = 0;
        int targetTop = 0;
        for (int page : expectedPages) {
            const int height = 480 + page * 160;
            QCOMPARE(strip.pixelColor(100, top + height / 2), colors[page]);
            if (page == target)
                targetTop = top;
            if (page != expectedPages.back())
                QCOMPARE(strip.pixelColor(100, top + height + 4).alpha(), 0);
            top += height + 8;
        }
        QCOMPARE(strip.size(), QSize(640, top - 8));
        auto* scroll = previewWindow()->findChild<QScrollArea*>();
        auto* bar = scroll->verticalScrollBar();
        QCOMPARE(bar->value(), std::clamp(targetTop + (480 + target * 160) / 2 - 24, 0, bar->maximum()));
        // Qt caps each wheel event to roughly one viewport of movement.
        for (int i = 0; i < 8; ++i)
            sendWheel(scroll->viewport(), 12000);
        QCOMPARE(bar->value(), 0);
        for (int i = 0; i < 8; ++i)
            sendWheel(scroll->viewport(), -12000);
        QCOMPARE(bar->value(), bar->maximum());
        QVERIFY(previews.showPreview(viewport));
        QCOMPARE(renderedPages, expectedPages); // All three pages reuse the cached strip.
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
        Generator::LinkPreview previews(
            [failedPage](int page, QSize size, const auto&) {
                if (page == failedPage)
                    return page == 2 ? solidPage(QSize(1, 1)) : QImage { };
                return solidPage(size, page == 1 ? Qt::green : Qt::red);
            },
            [](int page) { return LinkPreviewPage { page >= 0 && page < 3 ? QSizeF(640, 480) : QSizeF { } }; });
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination();
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        if (failedPage == 1) {
            auto* status = previewWindow()->findChild<QLabel*>();
            QTRY_COMPARE(status->text(), QStringLiteral("Preview unavailable"));
            QVERIFY(previewImage().isNull());
        } else {
            QTRY_COMPARE(previewImage().size(), QSize(640, 968));
            const int targetTop = failedPage == 0 ? 0 : 488;
            QCOMPARE(previewImage().pixelColor(100, targetTop + 100), QColor(Qt::green));
            auto* bar = previewWindow()->findChild<QScrollArea*>()->verticalScrollBar();
            QCOMPARE(bar->value(), std::max(0, targetTop - 24));
        }
    }

    void dismissCancelsPageBatch_data()
    {
        QTest::addColumn<int>("blockedPage");
        QTest::newRow("previous-page") << 0;
        QTest::newRow("destination-page") << 1;
        QTest::newRow("next-page") << 2;
    }

    void dismissCancelsPageBatch()
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
            [](int page) { return LinkPreviewPage { page >= 0 && page < 3 ? QSizeF(640, 480) : QSizeF { } }; });
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination();
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(blocked.load());
        QTest::keyClick(&m_host, Qt::Key_Escape);
        QTRY_VERIFY(cancelled.load());
        QVERIFY(!previewWindow());
        QCOMPARE(calls.load(), blockedPage + 1); // No subsequent page is rendered after cancellation.
        QTRY_VERIFY(previews.showPreview(viewport));
        QTRY_COMPARE(previewImage().size(), QSize(640, 1456));
        QCOMPARE(previewImage().pixelColor(100, 100), QColor(Qt::blue));
        QCOMPARE(calls.load(), blockedPage + 4);
    }

    void staysOpenAndScrollsUnderPointer()
    {
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
        auto* scroll = window->findChild<QScrollArea*>();
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

    void dragsPage_data()
    {
        QTest::addColumn<Qt::MouseButton>("button");
        QTest::addColumn<int>("distance");
        QTest::addColumn<int>("scrollDelta");
        QTest::newRow("left-up") << Qt::LeftButton << -80 << 80;
        QTest::newRow("left-down") << Qt::LeftButton << 80 << -80;
        QTest::newRow("right-ignored") << Qt::RightButton << -80 << 0;
        QTest::newRow("middle-ignored") << Qt::MiddleButton << -80 << 0;
    }

    void dragsPage()
    {
        QFETCH(Qt::MouseButton, button);
        QFETCH(int, distance);
        QFETCH(int, scrollDelta);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* scroll = window->findChild<QScrollArea*>();
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
        QCOMPARE(scroll->verticalScrollBar()->value(), initial + scrollDelta);
        QTest::mouseRelease(window->windowHandle(), button, Qt::NoModifier, start + QPoint(0, distance));
        released = true;
        QCOMPARE(scroll->viewport()->cursor().shape(), Qt::OpenHandCursor);
        QTest::mouseMove(window->windowHandle(), start);
        QCOMPARE(scroll->verticalScrollBar()->value(), initial + scrollDelta);
        QCOMPARE(m_host.clicks, 0);
        QVERIFY(window->isVisible());
    }

    void dragOutsideKeepsPreviewUntilFinished_data()
    {
        QTest::addColumn<bool>("loseGrab");
        QTest::newRow("released") << false;
        QTest::newRow("grab-lost") << true;
    }

    void dragOutsideKeepsPreviewUntilFinished()
    {
        QFETCH(bool, loseGrab);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
        auto* scroll = window->findChild<QScrollArea*>();
        const QPoint start =
            window->mapFromGlobal(scroll->viewport()->mapToGlobal(scroll->viewport()->rect().center()));
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
        QTest::mouseMove(window->windowHandle(), outside);
        QTest::qWait(450);
        QVERIFY(window->isVisible());
        if (loseGrab) {
            QEvent event(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(scroll->viewport(), &event);
        } else {
            QTest::mouseRelease(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, outside);
            released = true;
        }
        QTRY_VERIFY(!window->isVisible());
    }

    void interruptedDrag_data()
    {
        QTest::addColumn<bool>("loseGrab");
        QTest::newRow("dismissed") << false;
        QTest::newRow("grab-lost") << true;
    }

    void interruptedDrag()
    {
        QFETCH(bool, loseGrab);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 1600)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY(!previewImage().isNull());
        auto* window = previewWindow();
        auto* scroll = window->findChild<QScrollArea*>();
        const QPoint start =
            window->mapFromGlobal(scroll->viewport()->mapToGlobal(scroll->viewport()->rect().center()));
        QTest::mousePress(window->windowHandle(), Qt::LeftButton, Qt::NoModifier, start);
        const auto releaseMouse = qScopeGuard([&] { QTest::mouseRelease(m_host.windowHandle(), Qt::LeftButton); });
        QCOMPARE(scroll->viewport()->cursor().shape(), Qt::ClosedHandCursor);
        const int initial = scroll->verticalScrollBar()->value();
        if (loseGrab) {
            QEvent event(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(scroll->viewport(), &event);
            QTest::mouseMove(window->windowHandle(), start + QPoint(0, -80));
            QCOMPARE(scroll->verticalScrollBar()->value(), initial);
            QVERIFY(window->isVisible());
        } else {
            QTest::keyClick(&m_host, Qt::Key_Escape);
            QVERIFY(!window->isVisible());
        }
        QCOMPARE(scroll->viewport()->cursor().shape(), Qt::OpenHandCursor);
    }

    void dismissesOnInteraction_data()
    {
        QTest::addColumn<int>("activity");
        QTest::newRow("escape") << 0;
        QTest::newRow("outside-click") << 1;
        QTest::newRow("source-scroll") << 2;
        QTest::newRow("source-hidden") << 3;
        QTest::newRow("window-deactivated") << 4;
        QTest::newRow("document-reset") << 5;
    }

    void dismissesOnInteraction()
    {
        QFETCH(int, activity);
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        singlePage(QSizeF(560, 840)));
        previews.setMode(LinkPreviewMode::Large);
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QPointer<QWidget> window = previewWindow();
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
            [](int page) { return LinkPreviewPage { page >= 0 && page < 3 ? QSizeF(640, 480) : QSizeF { } }; });
        previews.setMode(LinkPreviewMode::Large);
        auto viewport = destination();
        viewport.page = 1;
        QVERIFY(previews.showPreview(viewport));
        QTRY_VERIFY(!previewImage().isNull());
        // The preview remains cached/displayed, but the transport lease is free.
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
        QTest::addColumn<QSize>("expected");
        QTest::newRow("wide") << QSizeF(1120, 560) << QSize(640, 320);
        QTest::newRow("extremely-wide") << QSizeF(1e300, 1e-300) << QSize(640, 1);
        QTest::newRow("extremely-tall") << QSizeF(1e-300, 1e300) << QSize(1, 4096);
    }

    void boundsUnusualGeometry()
    {
        QFETCH(QSizeF, geometry);
        QFETCH(QSize, expected);
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
        QVERIFY(previewImage().width() <= 640);
        QVERIFY(previewImage().height() <= 3 * 4096 + 16);
    }
};

QTEST_MAIN(TestLinkPreview)
#include "test_link_preview.moc"
