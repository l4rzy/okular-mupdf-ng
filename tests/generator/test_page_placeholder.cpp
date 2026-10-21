// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/page/placeholder.hpp"
#include <QTemporaryDir>
#include <QTest>
#include <limits>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

using Mu::Generator::Page::Placeholder;
using Mu::Model::DocumentTheme;

class TestPagePlaceholder : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.path().toUtf8());
        Okular::SettingsCore::instance(QStringLiteral("page-placeholder-test"));
    }

    void colors_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<QColor>("color");
        QTest::newRow("default") << int(DocumentTheme::Default) << QColor { };
        QTest::newRow("twilight") << int(DocumentTheme::Twilight) << QColor(208, 202, 184);
        QTest::newRow("dusk") << int(DocumentTheme::Dusk) << QColor(48, 49, 52);
        QTest::newRow("dark") << int(DocumentTheme::Dark) << QColor(32, 33, 36);
    }

    void colors()
    {
        QFETCH(int, theme);
        QFETCH(QColor, color);
        // Accessibility preferences must not be involved in preview generation.
        Okular::SettingsCore::setChangeColors(false);
        Okular::SettingsCore::setRenderMode(Okular::SettingsCore::EnumRenderMode::Inverted);
        Okular::SettingsCore::setPaperColor(Qt::yellow);
        Placeholder fallback;
        fallback.setTheme(static_cast<DocumentTheme>(theme));
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
        Placeholder fallback;
        Okular::DocumentObserver view;
        Okular::Page page(0, 600, 850, Okular::Rotation0);
        fallback.setTheme(DocumentTheme::Dark);
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
        fallback.setTheme(DocumentTheme::Dusk);
        fallback.seed(page);
        QVERIFY(page.hasPixmap(&view, 600, 850));
        fallback.setTheme(DocumentTheme::Default);
        fallback.seed(page);
        QVERIFY(!page.hasPixmap(&fallback));
        QVERIFY(page.hasPixmap(&view, 600, 850));
    }

    void refreshRemovesLatePreviews()
    {
        Placeholder placeholder;
        Okular::Page page(0, 600, 850, Okular::Rotation0);
        placeholder.setTheme(DocumentTheme::Dark);
        placeholder.seed(page);
        QVERIFY(page.hasPixmap(&placeholder, 256, 1));
        // Model a rotation job restoring a preview after Default was selected.
        placeholder.setTheme(DocumentTheme::Default);
        QVERIFY(page.hasPixmap(&placeholder));
        placeholder.refresh(page);
        QVERIFY(!page.hasPixmap(&placeholder));
        // Missing previews still recover when a theme is active.
        placeholder.setTheme(DocumentTheme::Dusk);
        placeholder.refresh(page);
        QVERIFY(page.hasPixmap(&placeholder, 256, 1));
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
        Placeholder fallback;
        fallback.setTheme(DocumentTheme::Dark);
        QCOMPARE(!fallback.partialImage(size).isNull(), accepted);
    }
};

QTEST_MAIN(TestPagePlaceholder)
#include "test_page_placeholder.moc"
