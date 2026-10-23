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

namespace {

Model::Viewport destination(double y = 0)
{
    Model::Viewport viewport;
    viewport.page = 0;
    viewport.normalizedY = y;
    viewport.coordinateMask = Model::Viewport::CoordinateY;
    return viewport;
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
            [](int) { return QSizeF(560, 840); });
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

    void staysOpenAndScrollsUnderPointer()
    {
        Generator::LinkPreview previews([](int, QSize size, const auto&) { return solidPage(size); },
                                        [](int) { return QSizeF(560, 1600); });
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
                                        [](int) { return QSizeF(560, 1600); });
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
                                        [](int) { return QSizeF(560, 1600); });
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
                                        [](int) { return QSizeF(560, 1600); });
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
                                        [](int) { return QSizeF(560, 840); });
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
            [](int) { return QSizeF(560, 840); });
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
        const auto released = std::make_shared<std::atomic_bool>(false);
        using Frame = std::pair<QImage, std::shared_ptr<std::atomic_bool>>;
        Generator::LinkPreview previews(
            [released](int, QSize size, const auto&) {
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
            [](int) { return QSizeF(560, 840); });
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        // The preview remains cached/displayed, but the transport lease is free.
        QVERIFY(released->load());
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
                                        [](int) { return QSizeF(560, 840); });
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
                                        [](int) { return QSizeF(560, 840); });
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
                                        [geometry](int) { return geometry; });
        QVERIFY(previews.showPreview(destination()));
        QTRY_VERIFY(!previewImage().isNull());
        QCOMPARE(previewImage().size(), expected);
    }

    void linksKeepNavigationAndExternalActions()
    {
        int pageQueries = 0;
        bool available = false;
        auto previews =
            std::make_unique<Generator::LinkPreview>([](int, QSize size, const auto&) { return solidPage(size); },
                                                     [&](int) {
                                                         ++pageQueries;
                                                         return available ? QSizeF(560, 840) : QSizeF { };
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
                const auto& geometry = pages.at(page).geometry;
                return QSizeF(geometry.widthPoints, geometry.heightPoints);
            });
        QVERIFY(previews.showPreview(destination(0.5)));
        QTRY_VERIFY_WITH_TIMEOUT(!previewImage().isNull(), 10000);
        QVERIFY(previewImage().width() <= 640);
        QVERIFY(previewImage().height() <= 4096);
    }
};

QTEST_MAIN(TestLinkPreview)
#include "test_link_preview.moc"
