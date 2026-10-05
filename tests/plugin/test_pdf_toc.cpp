// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <array>
#include <limits>
#include <stdexcept>
#include <unistd.h>

#include "plugin/caching/cache_file.hpp"
#include "plugin/caching/epub_cache.hpp"
#include "plugin/caching/pdf_toc_cache.hpp"
#include "worker/engine/pdf/generated_outline.hpp"

using namespace Mu;

namespace {

struct BookLayout {
    std::vector<Worker::Engine::OutlineLine> lines;

    void add(std::string title, int page, double x, double y, double size, double width, bool heading = true)
    {
        Worker::Engine::OutlineLine line;
        line.text = std::move(title);
        line.page = page;
        line.left = x;
        line.right = x + width;
        line.top = y;
        // Native font-metric boxes can overlap between consecutive title lines.
        line.bottom = y + size * 1.36;
        line.size = size;
        line.fontFamily = heading ? "Sans" : "Serif";
        line.boldFraction = heading ? 1 : 0;
        line.pageWidth = 600;
        line.pageHeight = 800;
        lines.push_back(std::move(line));
    }
};

} // namespace

class TestPdfToc : public QObject {
    Q_OBJECT
private slots:

    void parseHeading_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<int>("depth");
        QTest::newRow("chapter") << "1 Introduction" << true << 1;
        QTest::newRow("trailing-dot") << "1. Introduction" << true << 1;
        QTest::newRow("nested") << "1.2.3 Details" << true << 3;
        QTest::newRow("unicode") << "2 Résumé" << true << 1;
        QTest::newRow("whitespace") << "  10.2 Background  " << true << 2;
        QTest::newRow("bare") << "1.2" << false << 0;
        QTest::newRow("numeric-title") << "1.2 123" << false << 0;
        QTest::newRow("caption") << "Figure 1.2 Example" << false << 0;
        QTest::newRow("punctuation") << "1.2 ---" << false << 0;
        QTest::newRow("joined") << "1.2Introduction" << false << 0;
        QTest::newRow("overflow") << "9999999999999999 Introduction" << false << 0;
        QTest::newRow("too-deep") << "1.2.3.4.5.6.7.8.9 Heading" << false << 0;
        QTest::newRow("too-long") << "1 " + QString(161, 'a') << false << 0;
    }

    void parseHeading()
    {
        QFETCH(QString, text);
        QFETCH(bool, valid);
        QFETCH(int, depth);
        const auto heading = Worker::Engine::parseHeading(text.toStdString());
        QCOMPARE(heading.has_value(), valid);
        if (heading)
            QCOMPARE(heading->components.size(), static_cast<std::size_t>(depth));
    }

    void numberedHierarchy()
    {
        std::vector<Worker::Engine::OutlineLine> lines;
        const auto add = [&](std::string title, double size, double top) {
            Worker::Engine::OutlineLine line;
            line.text = title;
            line.size = size;
            line.fontFamily = "Body";
            line.top = top;
            line.bottom = top + size;
            line.left = 50;
            line.right = 400;
            line.pageWidth = 600;
            line.pageHeight = 800;
            lines.push_back(line);
        };
        add("Body prose with enough characters to establish a stable baseline.", 12, 100);
        add("1 Introduction", 18, 150);
        add("1.2 Background", 16, 200);
        add("1.2.3 Details", 15, 250);
        add("10 Other", 18, 300);
        add("10.2 Section", 16, 350);
        add("11.2 Missing parent", 16, 400);
        const auto nodes = Worker::Engine::buildGeneratedOutline(lines);
        QCOMPARE(nodes.size(), std::size_t(3));
        QCOMPARE(nodes[0].children[0].children[0].title, std::string("1.2.3 Details"));
        QCOMPARE(nodes[1].children[0].title, std::string("10.2 Section"));
        QCOMPARE(nodes[2].title, std::string("11.2 Missing parent"));
    }

    void reconstructBookLayout_data()
    {
        QTest::addColumn<bool>("printedContents");
        QTest::addColumn<QString>("contentsLabel");
        QTest::newRow("printed-contents") << true << "Contents";
        QTest::newRow("roman-prefix") << true << "viii Table of Contents";
        QTest::newRow("roman-suffix") << true << "Table of Contents ix";
        QTest::newRow("number-prefix") << true << "8 Table of Contents";
        QTest::newRow("number-suffix") << true << "Table of Contents 9";
        QTest::newRow("continued") << true << "Table of Contents (continued)";
        QTest::newRow("typography-fallback") << false << "";
    }

    void reconstructBookLayout()
    {
        QFETCH(bool, printedContents);
        QFETCH(QString, contentsLabel);
        BookLayout layout;
        if (printedContents) {
            layout.add(contentsLabel.toStdString(), 0, 50, 70, 24, 150);
            layout.add("Alpha Architecture", 0, 50, 130, 13, 150);
            layout.add("First Topic", 0, 50, 180, 11, 120);
            layout.add("1", 0, 240, 180, 11, 10);
            layout.add("Summary", 0, 50, 220, 11, 80);
            layout.add("1", 0, 240, 220, 11, 10);
            layout.add("Beta Architecture", 0, 330, 130, 13, 150);
            layout.add("Second Topic", 0, 330, 180, 11, 120);
            layout.add("2", 0, 550, 180, 11, 10);
            layout.add("Summary", 0, 330, 220, 11, 80);
            layout.add("2", 0, 550, 220, 11, 10);
        }
        for (int page : { 2, 3, 4 }) {
            layout.add("Running Header", page, 50, 20, 17, 150);
            layout.add(
                "An ordinary prose paragraph that establishes the document body style.", page, 50, 290, 11, 400, false);
        }
        layout.add("1", 2, 450, 160, 36, 30);
        layout.add("Alpha", 2, 380, 210, 32, 100);
        layout.add("Architecture", 2, 250, 245, 32, 230);
        layout.add("First Topic", 2, 50, 340, 17, 150);
        layout.add("Summary", 2, 50, 450, 17, 100);
        layout.add("2 Beta Architecture", 3, 50, 150, 32, 400);
        layout.add("Second Topic", 3, 50, 340, 17, 170);
        layout.add("Summary", 3, 50, 450, 17, 100);
        layout.add("2. Name the new template My Second Template.", 4, 50, 100, 11, 350, false);
        layout.add("Figure 2.1 Example", 4, 50, 400, 15, 200);
        layout.add("Note", 4, 50, 500, 16, 80);
        layout.add("for each package: print the name", 4, 50, 200, 10, 300, false);
        layout.lines.back().fontFamily = "Mono";
        layout.lines.back().monospaced = true;
        const auto nodes = Worker::Engine::buildGeneratedOutline(layout.lines);
        QCOMPARE(nodes.size(), std::size_t(2));
        QCOMPARE(nodes[0].title, std::string("1 Alpha Architecture"));
        QCOMPARE(nodes[0].link.viewport.page, 2);
        QCOMPARE(nodes[0].children.size(), std::size_t(2));
        QCOMPARE(nodes[0].children[0].title, std::string("First Topic"));
        QCOMPARE(nodes[0].children[1].title, std::string("Summary"));
        QCOMPARE(nodes[1].title, std::string("2 Beta Architecture"));
        QCOMPARE(nodes[1].children.size(), std::size_t(2));
        QCOMPARE(nodes[1].children[0].title, std::string("Second Topic"));
        QCOMPARE(nodes[1].children[1].title, std::string("Summary"));
    }

    void contentsColumnsAndWrappedTitles()
    {
        BookLayout layout;
        layout.add("Table of Contents", 0, 50, 70, 24, 200);
        layout.add("A wrapped", 0, 50, 150, 11, 190);
        layout.add("heading", 0, 50, 163, 11, 70);
        layout.add("1", 0, 250, 163, 11, 10);
        // This number is on the first line's baseline in the OTHER column.
        layout.add("Other title", 0, 350, 150, 11, 120);
        layout.add("2", 0, 550, 150, 11, 10);
        layout.add("Third title 3", 0, 350, 200, 11, 180);
        layout.add("Final title 4", 0, 350, 250, 11, 180);
        for (int page : { 2, 3, 4, 5 }) {
            layout.add("A sufficiently long paragraph of body prose for the typography baseline.",
                       page,
                       50,
                       300,
                       11,
                       400,
                       false);
        }
        layout.add("A wrapped heading", 2, 50, 150, 17, 250);
        layout.add("Other title", 3, 50, 150, 17, 200);
        layout.add("Third title", 4, 50, 150, 17, 200);
        layout.add("Final title", 5, 50, 150, 17, 200);
        // A typography-only result would also include this unlisted heading.
        layout.add("Unlisted callout", 5, 50, 500, 17, 200);
        const auto nodes = Worker::Engine::buildGeneratedOutline(layout.lines);
        QCOMPARE(nodes.size(), std::size_t(4));
        QCOMPARE(nodes[0].title, std::string("A wrapped heading"));
        QCOMPARE(nodes[0].link.viewport.page, 2);
        QCOMPARE(nodes[3].title, std::string("Final title"));
    }

    void contentsMentionIsNotContentsPage()
    {
        BookLayout layout;
        layout.add("Ordinary prose that establishes a stable body style.", 1, 50, 300, 11, 400, false);
        layout.add("Generating a table of contents", 1, 50, 150, 17, 300);
        layout.add("Another heading", 1, 50, 450, 17, 200);
        const auto nodes = Worker::Engine::buildGeneratedOutline(layout.lines);
        QCOMPARE(nodes.size(), std::size_t(2));
        QCOMPARE(nodes[0].title, std::string("Generating a table of contents"));
    }

    void incidentalContentsLabel_data()
    {
        QTest::addColumn<QString>("label");
        QTest::addColumn<double>("top");
        QTest::newRow("contents") << "Contents" << 250.0;
        QTest::newRow("table-of-contents") << "Table of Contents" << 250.0;
        QTest::newRow("continued") << "Table of Contents (continued)" << 250.0;
        QTest::newRow("running-header") << "Contents" << 20.0;
    }

    void incidentalContentsLabel()
    {
        QFETCH(QString, label);
        QFETCH(double, top);
        BookLayout layout;
        layout.add("Ordinary prose that establishes a stable body style.", 1, 50, 300, 11, 400, false);
        layout.add(label.toStdString(), 1, 50, top, 11, 200, false);
        layout.add("1 Overview", 1, 50, 150, 17, 200);
        layout.add("2 Results", 1, 50, 450, 17, 200);
        const auto nodes = Worker::Engine::buildGeneratedOutline(layout.lines);
        QCOMPARE(nodes.size(), std::size_t(2));
        QCOMPARE(nodes[0].title, std::string("1 Overview"));
        QCOMPARE(nodes[1].title, std::string("2 Results"));
        QCOMPARE(nodes[0].link.viewport.page, 1);
    }

    void malformedGeometry_data()
    {
        QTest::addColumn<int>("field");
        QTest::addColumn<double>("value");
        QTest::newRow("nan-size") << 0 << std::numeric_limits<double>::quiet_NaN();
        QTest::newRow("infinite-size") << 0 << std::numeric_limits<double>::infinity();
        QTest::newRow("zero-size") << 0 << 0.0;
        QTest::newRow("oversized-font") << 0 << 1000.0;
        QTest::newRow("zero-width") << 1 << 0.0;
        QTest::newRow("negative-height") << 2 << -1.0;
        QTest::newRow("nan-top") << 3 << std::numeric_limits<double>::quiet_NaN();
        QTest::newRow("inverted-box") << 4 << 0.0;
    }

    void malformedGeometry()
    {
        QFETCH(int, field);
        QFETCH(double, value);
        BookLayout layout;
        layout.add("Ordinary prose that establishes a stable body style.", 1, 50, 300, 11, 400, false);
        layout.add("Invalid heading", 1, 50, 150, 17, 200);
        auto& line = layout.lines.back();
        const std::array members { &Worker::Engine::OutlineLine::size,
                                   &Worker::Engine::OutlineLine::pageWidth,
                                   &Worker::Engine::OutlineLine::pageHeight,
                                   &Worker::Engine::OutlineLine::top,
                                   &Worker::Engine::OutlineLine::bottom };
        line.*members[static_cast<std::size_t>(field)] = value;
        layout.add("Valid heading", 1, 50, 450, 17, 200);
        const auto nodes = Worker::Engine::buildGeneratedOutline(layout.lines);
        QCOMPARE(nodes.size(), std::size_t(1));
        QCOMPARE(nodes[0].title, std::string("Valid heading"));
    }

    void reconstructionLimits_data()
    {
        QTest::addColumn<bool>("contents");
        QTest::newRow("contents-search-work") << true;
        QTest::newRow("heading-count") << false;
    }

    void reconstructionLimits()
    {
        QFETCH(bool, contents);
        BookLayout layout;
        if (contents) {
            layout.add("Contents", 0, 50, 70, 24, 150);
            // Thousands of overlapping numbers would otherwise force a
            // quadratic search despite the stored-line and node bounds.
            for (int i = 0; i < 1500; ++i) {
                layout.add("Overlapping contents title", 0, 50, 150, 11, 200);
                layout.add("1", 0, 500, 150, 11, 10);
            }
        } else {
            layout.add("Ordinary prose that establishes a stable body style.", 0, 50, 300, 11, 400, false);
            for (int i = 0; i < 5001; ++i)
                layout.add("Heading", i + 1, 50, 150, 17, 200);
        }
        QVERIFY_THROWS_EXCEPTION(std::length_error, Worker::Engine::buildGeneratedOutline(layout.lines));
    }

    void contentIdentityAndCacheValidation()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        Plugin::Caching::setRootForTesting(root.path());
        const auto clear = qScopeGuard([] { Plugin::Caching::clearRootForTesting(); });
        QFile source(root.filePath("source.pdf"));
        QVERIFY(source.open(QIODevice::ReadWrite));
        QCOMPARE(source.write("original contents"), qint64(17));
        QVERIFY(source.flush());
        const auto offset = ::lseek(source.handle(), 3, SEEK_SET);
        const QString path = Plugin::Caching::PDF::tocCachePath(source.handle());
        QVERIFY(!path.isEmpty());
        QCOMPARE(::lseek(source.handle(), 0, SEEK_CUR), offset);
        QVERIFY(!Plugin::Caching::PDF::loadToc(path, 2));
        Model::OutlineNode node;
        node.title = "1 Introduction";
        node.link.valid = true;
        node.link.viewport.page = 1;
        node.link.viewport.coordinateMask = Model::Viewport::CoordinateX | Model::Viewport::CoordinateY;
        node.link.viewport.normalizedY = 0.25;
        QVERIFY(Plugin::Caching::PDF::saveToc(path, 2, { node }));
        const auto loaded = Plugin::Caching::PDF::loadToc(path, 2);
        QVERIFY(loaded);
        QCOMPARE(loaded->front().title, node.title);
        QCOMPARE(loaded->front().link.viewport.normalizedY, 0.25);
        QVERIFY(!Plugin::Caching::PDF::loadToc(path, 1));
        node.link.viewport.normalizedY = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!Plugin::Caching::PDF::saveToc(path, 2, { node }));
        // Also reject invalid data written by the underlying generic container.
        node.link.viewport.normalizedY = 0.25;
        node.link.external = true;
        QVERIFY(Plugin::Caching::EPUB::Cache::saveOutlineAt(path, { node }));
        QVERIFY(!Plugin::Caching::PDF::loadToc(path, 2));
        QVERIFY(Plugin::Caching::PDF::saveToc(path, 2, { }));
        const auto empty = Plugin::Caching::PDF::loadToc(path, 2);
        QVERIFY(empty && empty->empty());
        QVERIFY(source.seek(0));
        QCOMPARE(source.write("modified contents"), qint64(17));
        QVERIFY(source.flush());
        QVERIFY(Plugin::Caching::PDF::tocCachePath(source.handle()) != path);
        QFile cache(path);
        QVERIFY(cache.open(QIODevice::WriteOnly));
        QCOMPARE(cache.write("broken"), qint64(6));
        cache.close();
        QVERIFY(!Plugin::Caching::PDF::loadToc(path, 2));
    }
};
QTEST_GUILESS_MAIN(TestPdfToc)
#include "test_pdf_toc.moc"
