// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/page/background.hpp"
#include <QTemporaryDir>
#include <QTest>
#include <limits>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

using Mu::Generator::Page::Background;
using Mu::Model::ReadingTheme;

class TestPageBackground : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.path().toUtf8());
        Okular::SettingsCore::instance(QStringLiteral("page-background-test"));
    }

    void colors_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<QColor>("color");
        QTest::newRow("default") << int(ReadingTheme::Default) << QColor { };
        QTest::newRow("twilight") << int(ReadingTheme::Twilight) << QColor(208, 202, 184);
        QTest::newRow("dusk") << int(ReadingTheme::Dusk) << QColor(48, 49, 52);
        QTest::newRow("dark") << int(ReadingTheme::Dark) << QColor(32, 33, 36);
    }

    void colors()
    {
        QFETCH(int, theme);
        QFETCH(QColor, color);
        // Accessibility preferences must not be involved in preview generation.
        Okular::SettingsCore::setChangeColors(false);
        Okular::SettingsCore::setRenderMode(Okular::SettingsCore::EnumRenderMode::Inverted);
        Okular::SettingsCore::setPaperColor(Qt::yellow);
        Background fallback;
        fallback.setTheme(static_cast<ReadingTheme>(theme));
        const auto image = fallback.partialImage({ 600, 850 });
        QCOMPARE(image.isNull(), !color.isValid());
        if (color.isValid()) {
            QCOMPARE(image.size(), QSize(600, 850));
            QCOMPARE(image.pixelColor(0, 0), color);
            QCOMPARE(image.pixelColor(599, 849), color);
        }
        QVERIFY(!Okular::SettingsCore::changeColors());
        QCOMPARE(Okular::SettingsCore::renderMode(), int(Okular::SettingsCore::EnumRenderMode::Inverted));
        QCOMPARE(Okular::SettingsCore::paperColor(), QColor(Qt::yellow));
    }

    void previewsDoNotCompleteViewRequests()
    {
        Background fallback;
        Okular::DocumentObserver view;
        Okular::Page page(0, 600, 850, Okular::Rotation0);
        fallback.setTheme(ReadingTheme::Dark);
        fallback.seed(page);
        QVERIFY(page.hasPixmap(&fallback));
        QVERIFY(!page.hasPixmap(&view));
        QVERIFY(!page.hasPixmap(&view, 256, 1));
        // Invalidation can remove the preview; reseeding must restore it.
        page.deletePixmaps();
        fallback.seed(page);
        QVERIFY(page.hasPixmap(&fallback));
        // Removing/changing the fallback must not remove a completed view image.
        page.setPixmap(&view, new QPixmap(600, 850));
        fallback.setTheme(ReadingTheme::Dusk);
        fallback.seed(page);
        QVERIFY(page.hasPixmap(&view, 600, 850));
        fallback.setTheme(ReadingTheme::Default);
        fallback.seed(page);
        QVERIFY(!page.hasPixmap(&fallback));
        QVERIFY(page.hasPixmap(&view, 600, 850));
    }

    void refreshRemovesLatePreviews()
    {
        Background background;
        Okular::Page page(0, 600, 850, Okular::Rotation0);
        background.setTheme(ReadingTheme::Dark);
        background.seed(page);
        QVERIFY(page.hasPixmap(&background, 256, 1));
        // Model a rotation job restoring a preview after Default was selected.
        background.setTheme(ReadingTheme::Default);
        QVERIFY(page.hasPixmap(&background));
        background.refresh(page);
        QVERIFY(!page.hasPixmap(&background));
        // Missing previews still recover when a theme is active.
        background.setTheme(ReadingTheme::Dusk);
        background.refresh(page);
        QVERIFY(page.hasPixmap(&background, 256, 1));
    }

    void boundedPartialImages_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("page") << QSize(600, 850) << true;
        QTest::newRow("tile") << QSize(401, 501) << true;
        QTest::newRow("empty") << QSize(0, 600) << false;
        QTest::newRow("negative") << QSize(600, -1) << false;
        QTest::newRow("oversized") << QSize(65534, 65534) << false;
        QTest::newRow("inclusive-tile-edge") << QSize(65535, 1) << true;
        QTest::newRow("wide-within-byte-budget") << QSize(65536, 1) << false;
        QTest::newRow("tall-within-byte-budget") << QSize(1, 65536) << false;
        QTest::newRow("overflow-product")
            << QSize(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()) << false;
    }

    void boundedPartialImages()
    {
        QFETCH(QSize, size);
        QFETCH(bool, accepted);
        Background fallback;
        fallback.setTheme(ReadingTheme::Dark);
        QCOMPARE(!fallback.partialImage(size).isNull(), accepted);
    }
};

QTEST_MAIN(TestPageBackground)
#include "test_page_background.moc"
