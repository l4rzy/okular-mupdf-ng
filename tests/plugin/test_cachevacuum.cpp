// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

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

    void removesOnlyStaleCacheFiles()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QDateTime now = utcNow();
        const QDateTime old = now.addDays(-100);

        const QString staleOcr =
            writeFile(root.filePath(QStringLiteral("ocr_cache/hash/p0_eng_300dpi.bin")), "old", old);
        const QString freshOcr =
            writeFile(root.filePath(QStringLiteral("ocr_cache/hash/p1_eng_300dpi.bin")), "new", now);
        const QString staleOnlyDoc = writeFile(root.filePath(QStringLiteral("ocr_cache/gone/p0_eng.bin")), "old", old);
        const QString staleEpub = writeFile(root.filePath(QStringLiteral("epub_accelerators/abc.bin")), "old", old);
        const QString freshEpub = writeFile(root.filePath(QStringLiteral("epub_accelerators/fresh.bin")), "new", now);
        const QString foreign = writeFile(root.filePath(QStringLiteral("other/keep.bin")), "old", old);
        for (const QString& path : { staleOcr, freshOcr, staleOnlyDoc, staleEpub, freshEpub, foreign })
            QVERIFY2(!path.isEmpty(), qPrintable(QStringLiteral("fixture setup failed")));
        QCOMPARE(QFileInfo(staleOcr).lastModified().toSecsSinceEpoch(), old.toSecsSinceEpoch());

        const auto result = Vacuum::vacuumStaleCaches(root.path(), now);
        QCOMPARE(result.filesRemoved, 3);
        QVERIFY(!QFile::exists(staleOcr));
        QVERIFY(!QFile::exists(staleOnlyDoc));
        QVERIFY(!QFile::exists(staleEpub));
        QVERIFY(QFile::exists(freshOcr));
        QVERIFY(QFile::exists(freshEpub));
        // Unknown trees are out of scope even when their files are old.
        QVERIFY(QFile::exists(foreign));
        // The emptied document directory is pruned; the live one survives.
        QVERIFY(!QDir(root.filePath(QStringLiteral("ocr_cache/gone"))).exists());
        QVERIFY(QDir(root.filePath(QStringLiteral("ocr_cache/hash"))).exists());
        QVERIFY(result.dirsRemoved >= 1);
    }

    void ignoresMissingTrees()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto result = Vacuum::vacuumStaleCaches(root.path(), utcNow());
        QCOMPARE(result.filesRemoved, 0);
        QCOMPARE(result.dirsRemoved, 0);
    }
};

QTEST_GUILESS_MAIN(TestCacheVacuum)

#include "test_cachevacuum.moc"
