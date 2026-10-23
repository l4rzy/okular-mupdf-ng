// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/document.hpp"
#include "generator/link_preview.hpp"
#include "plugin/worker_client.hpp"

#include <QApplication>
#include <QCursor>
#include <QPixmap>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTest>
#include <QTextDocument>
#include <QToolTip>
#include <QWheelEvent>
#include <QWidget>
#include <okular/core/page.h>

#include <atomic>
#include <limits>

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

QImage tooltipImage(const QString& html)
{
    const auto match = QRegularExpression(QStringLiteral("src=\"([^\"]+)\"")).match(html);
    if (!match.hasMatch())
        return { };
    // Exercise Qt's actual rich-text image loader, including data: URL support.
    QTextDocument document;
    document.setHtml(html);
    const auto resource = document.resource(QTextDocument::ImageResource, QUrl(match.captured(1)));
    return resource.metaType() == QMetaType::fromType<QImage>() ? resource.value<QImage>()
                                                                : resource.value<QPixmap>().toImage();
}

} // namespace

class TestLinkPreview : public QObject {
    Q_OBJECT

private slots:

    void destinationRegion_data()
    {
        QTest::addColumn<double>("y");
        QTest::addColumn<bool>("hasY");
        QTest::addColumn<int>("top");
        QTest::newRow("page-start") << 0.0 << true << 0;
        QTest::newRow("middle") << 0.5 << true << 396;
        QTest::newRow("page-end") << 1.0 << true << 520;
        QTest::newRow("clamped") << 2.0 << true << 520;
        QTest::newRow("unspecified") << 0.5 << false << 0;
        QTest::newRow("non-finite") << std::numeric_limits<double>::quiet_NaN() << true << 0;
    }

    void destinationRegion()
    {
        QFETCH(double, y);
        QFETCH(bool, hasY);
        QFETCH(int, top);
        QSize renderedSize;
        QRect renderedRegion;
        std::atomic_int calls = 0;
        Generator::LinkPreview previews(
            [&](int, QSize size, QRect region, const auto&) {
                ++calls;
                renderedSize = size;
                renderedRegion = region;
                QImage image(region.size(), QImage::Format_RGB32);
                image.fill(Qt::red);
                return image;
            },
            [](int) { return QSizeF(560, 840); });
        auto viewport = destination(y);
        if (!hasY)
            viewport.coordinateMask = 0;
        QVERIFY(!previews.tip(viewport).contains(QStringLiteral("data:image")));
        QString tip;
        QTRY_VERIFY((tip = previews.tip(viewport)).contains(QStringLiteral("data:image")));
        QCOMPARE(renderedSize, QSize(560, 840));
        QCOMPARE(renderedRegion, QRect(0, top, 560, 320));
        const QImage image = tooltipImage(tip);
        QCOMPARE(image.size(), QSize(560, 320));
        QCOMPARE(image.pixelColor(100, 100), QColor(Qt::red));
        QCOMPARE(previews.tip(viewport), tip);
        QCOMPARE(calls.load(), 1);
    }

    void resetCancelsAndDiscardsOldCompletion()
    {
        std::atomic_int calls = 0;
        std::atomic_bool started = false;
        Generator::LinkPreview previews(
            [&](int, QSize, QRect region, const auto& cancelled) {
                const int call = ++calls;
                started = true;
                if (call == 1) {
                    while (!cancelled())
                        QThread::msleep(1);
                }
                QImage image(region.size(), QImage::Format_RGB32);
                image.fill(call == 1 ? Qt::red : Qt::blue);
                return image;
            },
            [](int) { return QSizeF(560, 840); });
        previews.tip(destination());
        QTRY_VERIFY(started.load());
        // This also leaves the first job's completion queued on the GUI thread.
        previews.reset();
        QString tip;
        QTRY_VERIFY((tip = previews.tip(destination())).contains(QStringLiteral("data:image")));
        QCOMPARE(tooltipImage(tip).pixelColor(100, 100), QColor(Qt::blue));
        QCOMPARE(calls.load(), 2);
    }

    void updatesOnlyVisibleTooltip_data()
    {
        QTest::addColumn<int>("activity");
        QTest::newRow("still-hovered") << 0;
        QTest::newRow("hidden") << 1;
        QTest::newRow("pointer-moved-hide-pending") << 2;
        QTest::newRow("scrolled-hide-pending") << 3;
        QTest::newRow("window-deactivated") << 4;
    }

    void updatesOnlyVisibleTooltip()
    {
        QFETCH(int, activity);
        QWidget host;
        host.resize(640, 480);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        const QPoint position = host.mapToGlobal(QPoint(100, 100));
        QCursor::setPos(position);
        QTRY_COMPARE(QApplication::widgetAt(position), &host);
        std::atomic_bool release = false;
        Generator::LinkPreview previews(
            [&](int, QSize, QRect region, const auto& cancelled) {
                while (!release.load() && !cancelled())
                    QThread::msleep(1);
                QImage image(region.size(), QImage::Format_RGB32);
                image.fill(Qt::red);
                return image;
            },
            [](int) { return QSizeF(560, 840); });
        const QString pending = previews.tip(destination());
        QToolTip::showText(position, pending, &host, QRect(90, 90, 20, 20));
        QTRY_VERIFY(QToolTip::isVisible());
        if (activity == 1) {
            QToolTip::hideText();
            QTRY_VERIFY(!QToolTip::isVisible());
        } else if (activity == 2) {
            QTest::mouseMove(&host, QPoint(200, 200));
        } else if (activity == 3) {
            QWheelEvent wheel(QPointF(100, 100),
                              position,
                              { },
                              QPoint(0, 120),
                              Qt::NoButton,
                              Qt::NoModifier,
                              Qt::NoScrollPhase,
                              false);
            QApplication::sendEvent(&host, &wheel);
        } else if (activity == 4) {
            QEvent deactivate(QEvent::WindowDeactivate);
            QApplication::sendEvent(&host, &deactivate);
        }
        release = true;
        QTRY_VERIFY(previews.tip(destination()).contains(QStringLiteral("data:image")));
        if (activity != 0) {
            QVERIFY(!QToolTip::text().contains(QStringLiteral("data:image")));
        } else {
            QVERIFY(QToolTip::text().contains(QStringLiteral("data:image")));
            QTest::mouseMove(&host, QPoint(200, 200));
            QTRY_VERIFY(!QToolTip::isVisible());
        }
    }

    void boundsUnusualGeometry_data()
    {
        QTest::addColumn<QSizeF>("geometry");
        QTest::addColumn<QSize>("expected");
        QTest::newRow("wide") << QSizeF(1120, 560) << QSize(560, 280);
        QTest::newRow("extremely-wide") << QSizeF(1e300, 1e-300) << QSize(560, 1);
        QTest::newRow("extremely-tall") << QSizeF(1e-300, 1e300) << QSize(1, 4096);
    }

    void boundsUnusualGeometry()
    {
        QFETCH(QSizeF, geometry);
        QFETCH(QSize, expected);
        QSize renderedSize;
        Generator::LinkPreview previews(
            [&](int, QSize size, QRect region, const auto&) {
                renderedSize = size;
                QImage image(region.size(), QImage::Format_RGB32);
                image.fill(Qt::red);
                return image;
            },
            [geometry](int) { return geometry; });
        QTRY_VERIFY(previews.tip(destination()).contains(QStringLiteral("data:image")));
        QCOMPARE(renderedSize, expected);
    }

    void linksKeepNavigationAndExternalActions()
    {
        int pageQueries = 0;
        auto previews = std::make_unique<Generator::LinkPreview>(
            [](int, QSize, QRect, const auto&) { return QImage { }; },
            [&](int) {
                ++pageQueries;
                return QSizeF { }; // Unavailable document keeps the ordinary tip.
            });
        Model::Link internal;
        internal.left = 0.1;
        internal.top = 0.2;
        internal.right = 0.3;
        internal.bottom = 0.4;
        internal.target.valid = true;
        internal.target.viewport = destination(0.5);
        Model::Link external = internal;
        external.target.external = true;
        external.target.uri = "https://example.com/";
        auto rects = Generator::Conversion::objectRects({ internal, external }, previews.get());
        const auto cleanup = qScopeGuard([&] { qDeleteAll(rects); });
        QCOMPARE(rects.size(), 2);
        QCOMPARE(pageQueries, 0); // Constructing links performs no preview work.
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
        previews.reset();
        QCOMPARE(go->actionTip(), fallback); // Actions can outlive the controller.
    }

    void rendersThroughWorker()
    {
        Plugin::WorkerClient worker;
        QVERIFY(worker.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(worker.open(QStringLiteral(TEST_PDF_PATH), { }, pages), Model::OpenStatus::Success);
        QVERIFY(!pages.isEmpty());
        Generator::LinkPreview previews(
            [&](int page, QSize size, QRect region, const auto& cancelled) {
                return worker.render(page, size.width(), size.height(), region, cancelled);
            },
            [&](int page) {
                const auto& geometry = pages.at(page).geometry;
                return QSizeF(geometry.widthPoints, geometry.heightPoints);
            });
        QString tip;
        QTRY_VERIFY_WITH_TIMEOUT((tip = previews.tip(destination(0.5))).contains(QStringLiteral("data:image")), 10000);
        const QImage image = tooltipImage(tip);
        QVERIFY(!image.isNull());
        QVERIFY(image.width() <= 560);
        QVERIFY(image.height() <= 320);
    }
};

QTEST_MAIN(TestLinkPreview)
#include "test_link_preview.moc"
