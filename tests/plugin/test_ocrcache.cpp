// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

#include "plugin/caching/cache_file.hpp"
#include "plugin/caching/ocr_cache.hpp"
#include "plugin/caching/ocr_constants.hpp"
#include "plugin/ocr/constants.hpp"
#include "plugin/ocr/scheduler.hpp"

class TestOCRCache : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;
    const QString m_hash = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        ::Mu::Plugin::Caching::setRootForTesting(m_root.filePath("ocr-cache"));
    }

    void cleanupTestCase() { ::Mu::Plugin::Caching::clearRootForTesting(); }

    void cacheKeyNormalization()
    {
        // Null/empty inputs
        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::normalizeKey(QString(), QStringLiteral("eng"), 300));
        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::normalizeKey(m_hash, QString(), 300));

        // String with .traineddata and _300dpi suffix
        const auto key1 =
            ::Mu::Plugin::Caching::OCR::Cache::normalizeKey(m_hash, QStringLiteral("eng.traineddata"), 300);
        QVERIFY(key1.has_value());
        QCOMPARE(key1->documentHash, m_hash);
        QCOMPARE(key1->language, QStringLiteral("eng"));
        QCOMPARE(key1->dpi, 300);

        const auto key2 =
            ::Mu::Plugin::Caching::OCR::Cache::normalizeKey(m_hash, QStringLiteral("eng_150dpi.traineddata"), 0);
        QVERIFY(key2.has_value());
        QCOMPARE(key2->language, QStringLiteral("eng"));
        QCOMPARE(key2->dpi, 150);

        // Explicit DPI override
        const auto key3 = ::Mu::Plugin::Caching::OCR::Cache::normalizeKey(m_hash, QStringLiteral("eng_150dpi"), 300);
        QVERIFY(key3.has_value());
        QCOMPARE(key3->language, QStringLiteral("eng"));
        QCOMPARE(key3->dpi, 300);

        // Path equivalence
        QCOMPARE(::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(*key1, 5),
                 ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 5, QStringLiteral("eng_300dpi")));

        // Direct typed CacheKey operations
        const QVector<::Mu::Plugin::Caching::OCR::CacheItem> items { { QStringLiteral("K"), 0.1, 0.1, 0.2, 0.2 } };
        QVERIFY(::Mu::Plugin::Caching::OCR::Cache::save(*key1, 5, items));
        const auto loaded = ::Mu::Plugin::Caching::OCR::Cache::load(*key1, 5);
        QVERIFY(loaded.present);
        QCOMPARE(loaded.items.size(), 1);
        QCOMPARE(loaded.items.at(0).ch, QStringLiteral("K"));
    }

    void corruptionAndInvalidInputAreRemoved()
    {
        const QString path = ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 1, QStringLiteral("eng"));
        QVERIFY(::Mu::Plugin::Caching::OCR::Cache::save(m_hash, 1, QStringLiteral("eng"), { }));
        QCOMPARE(QFileInfo(path).size(), qint64(8));
        const auto emptyRes = ::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 1, QStringLiteral("eng"));
        QVERIFY(emptyRes.present);
        QVERIFY(emptyRes.items.isEmpty());
        QVERIFY(QFile::exists(path));
        QVERIFY(::Mu::Plugin::Caching::OCR::Cache::save(
            m_hash, 1, QStringLiteral("eng"), { { QStringLiteral("x"), 0, 0, .1, .1 } }));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("bad cache"), qint64(9));
        file.close();
        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 1, QStringLiteral("eng")).present);
        QVERIFY(!QFile::exists(path));
        QVERIFY(::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath({ }, 1, QStringLiteral("eng")).isEmpty());
    }

    void invalidCoordinatesAreRemoved()
    {
        const QString path = ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 2, QStringLiteral("eng"));
        QByteArray raw;
        QDataStream stream(&raw, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << QStringLiteral("x") << std::numeric_limits<double>::quiet_NaN() << 0.0 << 0.1 << 0.1;

        QByteArray fileData;
        QDataStream headerStream(&fileData, QIODevice::WriteOnly);
        headerStream.setVersion(QDataStream::Qt_6_0);
        headerStream << quint32(0x4F435232) << quint32(1);
        fileData.append(qCompress(raw));

        QFile file(path);
        QVERIFY(QFileInfo(path).dir().mkpath(QStringLiteral(".")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(fileData) > 0);
        file.close();

        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 2, QStringLiteral("eng")).present);
        QVERIFY(!QFile::exists(path));
    }

    void oversizedCacheFilesAreRemoved()
    {
        // Both tests exercise the load()-miss + purge path; there is no
        // Cache::exists() API, so both boundary sizes are covered here.
        const int pages[] = { 3, 7 };
        const qsizetype extras[] = { 10, 1 };
        for (int i = 0; i < 2; ++i) {
            const QString path =
                ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, pages[i], QStringLiteral("eng"));
            QFile file(path);
            QVERIFY(QFileInfo(path).dir().mkpath(QStringLiteral(".")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray data(
                static_cast<qsizetype>(::Mu::Plugin::OCR::Constant::MAX_CACHE_COMPRESSED_BYTES) + extras[i], '\0');
            QCOMPARE(file.write(data), qint64(data.size()));
            file.close();

            QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, pages[i], QStringLiteral("eng")).present);
            QVERIFY(!QFile::exists(path));
        }
    }

    void oversizedDeclaredPayloadIsRemoved()
    {
        const QString path = ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 4, QStringLiteral("eng"));
        QByteArray data = qCompress(QByteArray(64, '\0'));
        const quint32 declared = static_cast<quint32>(::Mu::Plugin::OCR::Constant::MAX_CACHE_RAW_BYTES) + 1;
        data[0] = char(declared >> 24);
        data[1] = char(declared >> 16);
        data[2] = char(declared >> 8);
        data[3] = char(declared);

        QByteArray fileData;
        QDataStream headerStream(&fileData, QIODevice::WriteOnly);
        headerStream.setVersion(QDataStream::Qt_6_0);
        headerStream << quint32(0x4F435232) << quint32(1);
        fileData.append(data);

        QFile file(path);
        QVERIFY(QFileInfo(path).dir().mkpath(QStringLiteral(".")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(fileData), qint64(fileData.size()));
        file.close();

        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 4, QStringLiteral("eng")).present);
        QVERIFY(!QFile::exists(path));
    }

    void decompressionBombIsRemoved()
    {
        const QString path = ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 5, QStringLiteral("eng"));
        QByteArray data =
            qCompress(QByteArray(static_cast<qsizetype>(::Mu::Plugin::OCR::Constant::MAX_CACHE_RAW_BYTES + 1), '\0'));
        data[0] = 0;
        data[1] = 0;
        data[2] = 0;
        data[3] = 4;

        QByteArray fileData;
        QDataStream headerStream(&fileData, QIODevice::WriteOnly);
        headerStream.setVersion(QDataStream::Qt_6_0);
        headerStream << quint32(0x4F435232) << quint32(1);
        fileData.append(data);

        QFile file(path);
        QVERIFY(QFileInfo(path).dir().mkpath(QStringLiteral(".")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(fileData), qint64(fileData.size()));
        file.close();

        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 5, QStringLiteral("eng")).present);
        QVERIFY(!QFile::exists(path));
    }

    void hashIsTheCacheIdentity()
    {
        const QVector<::Mu::Plugin::Caching::OCR::CacheItem> items {
            { QStringLiteral("H"), .1, .1, .2, .2 },
            { QStringLiteral("i"), .2, .1, .3, .2 },
        };
        const QString path =
            ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 6, QStringLiteral("eng_300dpi"));
        QVERIFY(path.startsWith(m_root.path()));
        QVERIFY(::Mu::Plugin::Caching::OCR::Cache::save(m_hash, 6, QStringLiteral("eng_300dpi"), items));
        QVERIFY(QFile::exists(path));
        QCOMPARE(::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 6, QStringLiteral("eng_300dpi")), path);
        QVERIFY(!path.contains(QStringLiteral(".pdf")));
        QVERIFY(path
                != ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(
                    QStringLiteral("different-hash"), 6, QStringLiteral("eng_300dpi")));

        const auto res = ::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 6, QStringLiteral("eng_150dpi"));
        QVERIFY(res.present);
        QCOMPARE(res.items.size(), 2);
        QCOMPARE(res.items.at(0).ch + res.items.at(1).ch, QStringLiteral("Hi"));
        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 6, QStringLiteral("deu_150dpi")).present);
        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 7, QStringLiteral("eng_150dpi")).present);
    }

    void typedDpiIsolation()
    {
        const QVector<::Mu::Plugin::Caching::OCR::CacheItem> items {
            { QStringLiteral("A"), 0.1, 0.1, 0.2, 0.2 },
        };
        const QString path300 =
            ::Mu::Plugin::Caching::OCR::Cache::getCacheFilePath(m_hash, 8, QStringLiteral("eng"), 300);
        QVERIFY(::Mu::Plugin::Caching::OCR::Cache::save(m_hash, 8, QStringLiteral("eng"), 300, items));
        QVERIFY(QFile::exists(path300));

        // Loading or checking exists with 150dpi resolves 300dpi cache
        QVERIFY(::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 8, QStringLiteral("eng"), 150).present);
        const auto res = ::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 8, QStringLiteral("eng"), 150);
        QVERIFY(res.present);
        QCOMPARE(res.items.size(), 1);
        QCOMPARE(res.items.at(0).ch, QStringLiteral("A"));

        // Language isolation remains intact
        QVERIFY(!::Mu::Plugin::Caching::OCR::Cache::load(m_hash, 8, QStringLiteral("deu"), 150).present);
    }

    void schedulerUsesSettledPageAndDirection()
    {
        ::Mu::Plugin::OCR::Scheduler scheduler;
        QVERIFY(scheduler.observe(10, 100));
        QVERIFY(!scheduler.observe(10, 100));
        QVERIFY(scheduler.observe(52, 100));
        QCOMPARE(scheduler.settle(), QList<int>({ 52, 53, 51 }));

        scheduler.observe(10, 100);
        QCOMPARE(scheduler.settle(), QList<int>({ 10, 9, 11 }));
    }

    void schedulerHandlesDocumentEdges()
    {
        ::Mu::Plugin::OCR::Scheduler scheduler;
        scheduler.observe(0, 2);
        QCOMPARE(scheduler.settle(), QList<int>({ 0, 1 }));
        scheduler.observe(1, 2);
        QCOMPARE(scheduler.settle(), QList<int>({ 1, 0 }));

        scheduler.reset();
        scheduler.observe(0, 1);
        QCOMPARE(scheduler.settle(), QList<int>({ 0 }));
    }

    void schedulerDebouncesRapidScrollingAndCancelsOldWork()
    {
        ::Mu::Plugin::OCR::Scheduler scheduler;
        scheduler.observe(10, 100);
        QCOMPARE(scheduler.settle(), QList<int>({ 10, 11, 9 }));

        // No work is scheduled while the view is still moving.  This models
        // the controller's debounce timer receiving a rapid stream of pages.
        for (int i = 0; i < 1000; ++i)
            QVERIFY(scheduler.observe(11 + i % 41, 100));
        QVERIFY(scheduler.observe(52, 100));

        const auto pages = scheduler.settle();
        QCOMPARE(pages, QList<int>({ 52, 53, 51 }));
        QVERIFY(scheduler.shouldCancelRunning(10));
        QVERIFY(!scheduler.shouldCancelRunning(52));
    }

    void dominantPageUsesVisibleArea()
    {
        const QList<::Mu::Plugin::OCR::VisiblePage> pages {
            { 10, 0.7 * 600 * 800 },
            { 11, 0.6 * 1000 * 1000 },
        };
        QCOMPARE(::Mu::Plugin::OCR::dominantPage(pages), 11);

        const QList<::Mu::Plugin::OCR::VisiblePage> tied {
            { 4, 100 },
            { 5, 100 },
        };
        QCOMPARE(::Mu::Plugin::OCR::dominantPage(tied), 4);
        QCOMPARE(::Mu::Plugin::OCR::dominantPage(tied, 5), 5);
    }
};

QTEST_GUILESS_MAIN(TestOCRCache)

#include "test_ocrcache.moc"
