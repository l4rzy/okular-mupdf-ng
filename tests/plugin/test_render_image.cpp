// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QImage>
#include <QTest>

#include "plugin/util/render_image.hpp"

using Mu::Plugin::Util::normalizeRenderImage;

class TestPluginRenderImage : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void nullAndInvalidPassThrough()
    {
        QVERIFY(normalizeRenderImage(QImage(), QSize(10, 10)).isNull());

        const QImage source(4, 4, QImage::Format_RGBA8888);
        QCOMPARE(normalizeRenderImage(source, QSize()).size(), source.size());
        QCOMPARE(normalizeRenderImage(source, QSize(-1, 4)).size(), source.size());
    }

    void exactSizePassesThrough()
    {
        QImage source(8, 6, QImage::Format_RGBA8888);
        source.fill(Qt::red);
        const QImage result = normalizeRenderImage(source, QSize(8, 6));
        QCOMPARE(result.size(), QSize(8, 6));
    }

    void fittedFrameScalesToExpectedSize()
    {
        QImage source(4, 4, QImage::Format_RGBA8888);
        source.fill(Qt::green);
        QCOMPARE(normalizeRenderImage(source, QSize(8, 8)).size(), QSize(8, 8));
        QCOMPARE(normalizeRenderImage(QImage(8, 8, QImage::Format_RGBA8888), QSize(4, 4)).size(), QSize(4, 4));
    }

    void inclusiveEdgePixelScalesToExpectedSize()
    {
        // Okular geometry is inclusive on the right/bottom edge, so an
        // edge-clipped worker tile can be one pixel smaller per axis.
        QCOMPARE(normalizeRenderImage(QImage(4, 5, QImage::Format_RGBA8888), QSize(5, 5)).size(), QSize(5, 5));
        QCOMPARE(normalizeRenderImage(QImage(5, 4, QImage::Format_RGBA8888), QSize(5, 5)).size(), QSize(5, 5));
        QCOMPARE(normalizeRenderImage(QImage(4, 4, QImage::Format_RGBA8888), QSize(5, 5)).size(), QSize(5, 5));
    }
};

QTEST_MAIN(TestPluginRenderImage)

#include "test_render_image.moc"
