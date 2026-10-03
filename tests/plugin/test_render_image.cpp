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

        QImage source(4, 4, QImage::Format_RGBA8888);
        source.fill(Qt::red);
        QCOMPARE(normalizeRenderImage(source, QSize()).cacheKey(), source.cacheKey());
        QCOMPARE(normalizeRenderImage(source, QSize(-1, 4)).cacheKey(), source.cacheKey());
    }

    void exactSizePassesThrough()
    {
        QImage source(8, 6, QImage::Format_RGBA8888);
        source.fill(Qt::red);
        const QImage result = normalizeRenderImage(source, QSize(8, 6));
        QCOMPARE(result.cacheKey(), source.cacheKey());
        QCOMPARE(result, source);
    }

    void scalesFramesWithoutLosingPixels_data()
    {
        QTest::addColumn<QSize>("sourceSize");
        QTest::addColumn<QSize>("expectedSize");
        QTest::newRow("upscale") << QSize(4, 4) << QSize(8, 8);
        QTest::newRow("downscale") << QSize(8, 8) << QSize(4, 4);
        QTest::newRow("right-edge") << QSize(4, 5) << QSize(5, 5);
        QTest::newRow("bottom-edge") << QSize(5, 4) << QSize(5, 5);
        QTest::newRow("both-edges") << QSize(4, 4) << QSize(5, 5);
    }

    void scalesFramesWithoutLosingPixels()
    {
        QFETCH(QSize, sourceSize);
        QFETCH(QSize, expectedSize);
        QImage source(sourceSize, QImage::Format_RGBA8888);
        source.fill(Qt::green);
        const auto result = normalizeRenderImage(source, expectedSize);
        QCOMPARE(result.size(), expectedSize);
        for (int y = 0; y < result.height(); ++y)
            for (int x = 0; x < result.width(); ++x)
                QCOMPARE(result.pixelColor(x, y), QColor(Qt::green));
    }
};

QTEST_MAIN(TestPluginRenderImage)

#include "test_render_image.moc"
