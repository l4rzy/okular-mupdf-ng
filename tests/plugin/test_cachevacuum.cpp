// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <unistd.h>
#include <utime.h>

#include "plugin/caching/vacuum.hpp"

namespace Vacuum = ::Mu::Plugin::Caching::Vacuum;

class TestCacheVacuum : public QObject {
    Q_OBJECT

private:
    static QDateTime utcNow() { return QDateTime::currentDateTimeUtc(); }

    static QString writeFile(const QString& path, const QByteArray& contents, const QDateTime& mtime)
    {
        if (!QFileInfo(path).dir().mkpath(QStringLiteral(".")))
            return { };
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
            return { };
        file.close();
        // QFile::setFileTime proved unreliable on the test host (reports
        // success without changing mtime), so stamp via POSIX utime.
        struct utimbuf times { };
        times.actime = mtime.toSecsSinceEpoch();
        times.modtime = mtime.toSecsSinceEpoch();
        if (::utime(path.toLocal8Bit().constData(), &times) != 0)
            return { };
        return path;
    }

private slots:

    void vacuumsSelectedCache_data()
    {
        QTest::addColumn<Vacuum::CacheKind>("kind");
        QTest::addColumn<QString>("selected");
        QTest::addColumn<bool>("manual");
        QTest::newRow("ocr") << Vacuum::CacheKind::Ocr << QStringLiteral("ocr") << false;
        QTest::newRow("epub") << Vacuum::CacheKind::EpubAccelerator << QStringLiteral("epub_accel") << false;
        QTest::newRow("toc") << Vacuum::CacheKind::PdfToc << QStringLiteral("pdf_toc") << false;
        QTest::newRow("all") << Vacuum::CacheKind::All << QString() << false;
        QTest::newRow("manual-ocr") << Vacuum::CacheKind::Ocr << QStringLiteral("ocr") << true;
        QTest::newRow("manual-epub") << Vacuum::CacheKind::EpubAccelerator << QStringLiteral("epub_accel") << true;
        QTest::newRow("manual-toc") << Vacuum::CacheKind::PdfToc << QStringLiteral("pdf_toc") << true;
        QTest::newRow("manual-all") << Vacuum::CacheKind::All << QString() << true;
    }

    void vacuumsSelectedCache()
    {
        QFETCH(Vacuum::CacheKind, kind);
        QFETCH(QString, selected);
        QFETCH(bool, manual);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto now = utcNow();
        const auto old = now.addDays(-100);
        const QStringList trees { "ocr", "epub_accel", "pdf_toc" };
        for (const auto& tree : trees) {
            QVERIFY(!writeFile(root.filePath(tree + "/.expired/deep/.old"), "old", old).isEmpty());
            const auto freshTime = tree == "pdf_toc" ? now.addDays(1) : now;
            QVERIFY(!writeFile(root.filePath(tree + "/fresh"), tree.toUtf8(), freshTime).isEmpty());
        }
        const auto foreign = writeFile(root.filePath("other/keep"), "ignored", old);
        QVERIFY(!foreign.isEmpty());
        const auto before = Vacuum::measureCacheUsage(root.path());
        QCOMPARE(before.errors, 0);
        QCOMPARE(before.ocrBytes, 6);
        QCOMPARE(before.epubAcceleratorBytes, 13);
        QCOMPARE(before.pdfTocBytes, 10);
        QCOMPARE(before.totalBytes(), 29);

        const auto result =
            manual ? Vacuum::clearCaches(root.path(), kind) : Vacuum::vacuumStaleCaches(root.path(), now, kind);
        QCOMPARE(result.errors, 0);
        QCOMPARE(result.filesRemoved, (selected.isEmpty() ? 3 : 1) * (manual ? 2 : 1));
        const auto after = Vacuum::measureCacheUsage(root.path());
        QCOMPARE(after.errors, 0);
        QCOMPARE(after.ocrBytes, selected.isEmpty() || selected == "ocr" ? (manual ? 0 : 3) : 6);
        QCOMPARE(after.epubAcceleratorBytes, selected.isEmpty() || selected == "epub_accel" ? (manual ? 0 : 10) : 13);
        QCOMPARE(after.pdfTocBytes, selected.isEmpty() || selected == "pdf_toc" ? (manual ? 0 : 7) : 10);
        for (const auto& tree : trees) {
            const bool retained = !selected.isEmpty() && tree != selected;
            QCOMPARE(QFile::exists(root.filePath(tree + "/.expired/deep/.old")), retained);
            QCOMPARE(QDir(root.filePath(tree + "/.expired")).exists(), retained);
            QCOMPARE(QFile::exists(root.filePath(tree + "/fresh")), !manual || retained);
            QVERIFY(QDir(root.filePath(tree)).exists());
        }
        QVERIFY(QFile::exists(foreign));
    }

    void skipsSymlinks_data()
    {
        QTest::addColumn<QString>("linkPath");
        QTest::addColumn<bool>("linkFile");
        QTest::newRow("file") << QStringLiteral("ocr/file") << true;
        QTest::newRow("nested-directory") << QStringLiteral("ocr/linked") << false;
        QTest::newRow("cache-directory") << QStringLiteral("ocr") << false;
        QTest::newRow("cache-root") << QStringLiteral("linked-root") << false;
    }

    void skipsSymlinks()
    {
        QFETCH(QString, linkPath);
        QFETCH(bool, linkFile);
        QTemporaryDir root;
        QTemporaryDir outside;
        QVERIFY(root.isValid());
        QVERIFY(outside.isValid());
        const auto now = utcNow();
        const QString targetFile = writeFile(outside.filePath("ocr/old"), "outside", now.addDays(-100));
        QVERIFY(!targetFile.isEmpty());
        const QString link = root.filePath(linkPath);
        QVERIFY(QFileInfo(link).dir().mkpath("."));
        const bool rootLink = linkPath == "linked-root";
        QVERIFY(QFile::link(linkFile ? targetFile : rootLink ? outside.path() : outside.filePath("ocr"), link));
        const QString scanRoot = rootLink ? link + '/' : root.path();
        QCOMPARE(Vacuum::measureCacheUsage(scanRoot).totalBytes(), 0);
        QCOMPARE(Vacuum::vacuumStaleCaches(scanRoot, now).filesRemoved, 0);
        QCOMPARE(Vacuum::clearCaches(scanRoot).filesRemoved, 0);
        QVERIFY(QFile::exists(targetFile));
        QVERIFY(QFileInfo(link).isSymLink());
    }

    void reportsRemovalFailure()
    {
        if (::geteuid() == 0)
            QSKIP("Root bypasses directory write permissions");
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto now = utcNow();
        const QString file = writeFile(root.filePath("ocr/old"), "old", now.addDays(-100));
        QVERIFY(!file.isEmpty());
        const QString directory = root.filePath("ocr");
        const auto permissions = QFile::permissions(directory);
        const auto restore = qScopeGuard([&] { QFile::setPermissions(directory, permissions); });
        QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
        const auto result = Vacuum::vacuumStaleCaches(root.path(), now);
        QCOMPARE(result.filesRemoved, 0);
        QCOMPARE(result.errors, 1);
        QVERIFY(QFile::exists(file));
    }

    void reportsInvalidCacheDirectory_data()
    {
        QTest::addColumn<bool>("invalidRoot");
        QTest::newRow("root") << true;
        QTest::newRow("tree") << false;
    }

    void reportsInvalidCacheDirectory()
    {
        QFETCH(bool, invalidRoot);
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const auto now = utcNow();
        const auto file = writeFile(parent.filePath("ocr"), "unexpected", now.addDays(-100));
        QVERIFY(!file.isEmpty());
        const auto root = invalidRoot ? file : parent.path();
        const auto usage = Vacuum::measureCacheUsage(root);
        QCOMPARE(usage.totalBytes(), 0);
        QCOMPARE(usage.errors, 1);
        const auto cleanup = Vacuum::vacuumStaleCaches(root, now);
        QCOMPARE(cleanup.filesRemoved, 0);
        QCOMPARE(cleanup.errors, 1);
        QVERIFY(QFile::exists(file));
    }

    void reportsInaccessibleCache_data()
    {
        QTest::addColumn<QString>("restrictedPath");
        QTest::addColumn<int>("permissions");
        QTest::newRow("unreadable-root") << QString() << int(QFile::ExeOwner);
        QTest::newRow("unsearchable-root") << QString() << int(QFile::ReadOwner);
        QTest::newRow("unreadable-tree") << QStringLiteral("ocr") << int(QFile::ExeOwner);
        QTest::newRow("unsearchable-tree") << QStringLiteral("ocr") << int(QFile::ReadOwner);
        QTest::newRow("unreadable-nested") << QStringLiteral("ocr/nested") << int(QFile::ExeOwner);
        QTest::newRow("unsearchable-nested") << QStringLiteral("ocr/nested") << int(QFile::ReadOwner);
    }

    void reportsInaccessibleCache()
    {
        if (::geteuid() == 0)
            QSKIP("Root bypasses directory permissions");
        QFETCH(QString, restrictedPath);
        QFETCH(int, permissions);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto now = utcNow();
        const auto file = writeFile(root.filePath("ocr/nested/old"), "old", now.addDays(-100));
        QVERIFY(!file.isEmpty());
        const auto path = restrictedPath.isEmpty() ? root.path() : root.filePath(restrictedPath);
        const auto original = QFile::permissions(path);
        const auto restore = qScopeGuard([&] { QFile::setPermissions(path, original); });
        QVERIFY(QFile::setPermissions(path, QFile::Permissions(permissions)));
        QVERIFY(Vacuum::measureCacheUsage(root.path()).errors > 0);
        const auto cleanup = Vacuum::vacuumStaleCaches(root.path(), now);
        QVERIFY(cleanup.errors > 0);
        QCOMPARE(cleanup.filesRemoved, 0);
        QVERIFY(QFile::setPermissions(path, original));
        QVERIFY(QFile::exists(file));
    }

    void parsesStoredTimestamp()
    {
        QVERIFY(!Vacuum::parseLastVacuum(QString()).isValid());
        QVERIFY(!Vacuum::parseLastVacuum(QStringLiteral("not-a-time")).isValid());

        const QDateTime moment(QDate(2026, 3, 4), QTime(5, 6, 7), QTimeZone::UTC);
        const QDateTime roundTripped = Vacuum::parseLastVacuum(Vacuum::formatLastVacuum(moment));
        QVERIFY(roundTripped.isValid());
        QCOMPARE(roundTripped, moment);
    }

    void throttlesByStoredTimestamp()
    {
        const QDateTime now = utcNow();
        // Never vacuumed, or clock skew into the future: vacuum once.
        QVERIFY(Vacuum::shouldVacuum(QDateTime { }, now));
        QVERIFY(Vacuum::shouldVacuum(now.addDays(1), now));
        // A week-old timestamp re-arms; anything newer stays quiet.
        QVERIFY(Vacuum::shouldVacuum(now.addDays(-7), now));
        QVERIFY(Vacuum::shouldVacuum(now.addDays(-30), now));
        QVERIFY(!Vacuum::shouldVacuum(now, now));
        QVERIFY(!Vacuum::shouldVacuum(now.addDays(-6), now));
    }

    void detectsStaleMtime()
    {
        const QDateTime now = utcNow();
        QVERIFY(!Vacuum::isStale(QDateTime { }, now));
        QVERIFY(!Vacuum::isStale(now.addDays(1), now));
        QVERIFY(!Vacuum::isStale(now, now));
        QVERIFY(!Vacuum::isStale(now.addDays(-89), now));
        QVERIFY(Vacuum::isStale(now.addDays(-90), now));
        QVERIFY(Vacuum::isStale(now.addDays(-120), now));
    }

    void ignoresMissingTrees()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto usage = Vacuum::measureCacheUsage(root.path());
        QCOMPARE(usage.totalBytes(), 0);
        QCOMPARE(usage.errors, 0);
        const auto result = Vacuum::vacuumStaleCaches(root.path(), utcNow());
        QCOMPARE(result.errors, 0);
        QCOMPARE(result.filesRemoved, 0);
        QCOMPARE(result.dirsRemoved, 0);
    }
};

QTEST_GUILESS_MAIN(TestCacheVacuum)

#include "test_cachevacuum.moc"
