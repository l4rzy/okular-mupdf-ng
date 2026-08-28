// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QImage>
#include <QTest>

#include "generator/placeholder_image.hpp"

class TestPlaceholderImage : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void testInvalidDimensions()
    {
        QVERIFY(::Mu::Generator::createPlaceholderImage(0, 100, ::Mu::Generator::PlaceholderKind::Error).isNull());
        QVERIFY(::Mu::Generator::createPlaceholderImage(100, 0, ::Mu::Generator::PlaceholderKind::Error).isNull());
        QVERIFY(::Mu::Generator::createPlaceholderImage(-10, -20, ::Mu::Generator::PlaceholderKind::Error).isNull());
    }

    void testThumbnailRendering()
    {
        // Small thumbnails (< 120x80) render the card but omit text
        const auto img = ::Mu::Generator::createPlaceholderImage(80, 60, ::Mu::Generator::PlaceholderKind::Error);
        QCOMPARE(img.width(), 80);
        QCOMPARE(img.height(), 60);
        QVERIFY(!img.isNull());
    }

    void testStandardRenderingAllKinds()
    {
        const ::Mu::Generator::PlaceholderKind kinds[] = {
            ::Mu::Generator::PlaceholderKind::Error,
            ::Mu::Generator::PlaceholderKind::Loading,
        };

        for (const auto kind : kinds) {
            const auto img = ::Mu::Generator::createPlaceholderImage(600, 800, kind);
            QCOMPARE(img.width(), 600);
            QCOMPARE(img.height(), 800);
            QCOMPARE(img.format(), QImage::Format_ARGB32_Premultiplied);
            QVERIFY(!img.isNull());
        }
    }

    void testCustomMessage()
    {
        const auto img = ::Mu::Generator::createPlaceholderImage(
            400, 300, ::Mu::Generator::PlaceholderKind::Loading, QStringLiteral("Custom loading message"));
        QCOMPARE(img.width(), 400);
        QCOMPARE(img.height(), 300);
        QVERIFY(!img.isNull());
    }
};

QTEST_MAIN(TestPlaceholderImage)

#include "test_placeholder_image.moc"
