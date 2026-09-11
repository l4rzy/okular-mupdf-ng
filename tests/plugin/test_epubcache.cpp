// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDataStream>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "plugin/caching/cache_file.hpp"
#include "plugin/caching/epub_cache.hpp"

class TestEpubCache : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;
    QString m_documentPath;

    QString cacheFile() const
    {
        QDirIterator iterator(m_root.path(), { QStringLiteral("*.bin") }, QDir::Files, QDirIterator::Subdirectories);
        return iterator.hasNext() ? iterator.next() : QString();
    }

    void clearCacheFiles() { QVERIFY(QDir(m_root.filePath(QStringLiteral("cache"))).removeRecursively()); }

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        m_documentPath = m_root.filePath(QStringLiteral("sample.epub"));
        QFile document(m_documentPath);
        QVERIFY(document.open(QIODevice::WriteOnly));
        QCOMPARE(document.write("epub source"), qint64(11));
        document.close();
        ::Mu::Plugin::Caching::setRootForTesting(m_root.filePath(QStringLiteral("cache")));
    }

    void cleanupTestCase() { ::Mu::Plugin::Caching::clearRootForTesting(); }

    void roundTripAndSettingsIsolation()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;
        const QByteArray accelerator("accelerator bytes");
        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, accelerator));
        ::Mu::Model::OutlineNode chapter;
        chapter.title = "Chapter 1";
        chapter.open = true;
        chapter.link.viewport.page = 3;
        chapter.link.valid = true;
        ::Mu::Model::OutlineNode section;
        section.title = "Section";
        chapter.children.push_back(section);
        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::saveOutline(m_documentPath, settings, { chapter }));
        const auto loaded = ::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings);
        QVERIFY(loaded);
        QVERIFY(loaded->accelerator);
        QCOMPARE(*loaded->accelerator, accelerator);
        QVERIFY(loaded->outline);
        QCOMPARE(loaded->outline->size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(loaded->outline->front().title), QStringLiteral("Chapter 1"));
        QCOMPARE(loaded->outline->front().children.size(), std::size_t(1));
        QCOMPARE(loaded->outline->front().link.viewport.page, 3);
        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, QByteArray("updated")));
        const auto preserved = ::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings);
        QVERIFY(preserved && preserved->outline);
        QCOMPARE(preserved->outline->front().title, std::string("Chapter 1"));

        settings.epub.fontSize++;
        QVERIFY(!::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings));
    }

    void preservesValidEmptyOutline()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;
        settings.epub.fontSize++;
        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::saveOutline(m_documentPath, settings, { }));
        const auto loaded = ::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings);
        QVERIFY(loaded);
        QVERIFY(!loaded->accelerator);
        QVERIFY(loaded->outline);
        QVERIFY(loaded->outline->empty());
    }

    void invalidatesSourceChangesAndRemovesCorruption()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;
        QVERIFY(
            ::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, QByteArray("accelerator")));
        const QString path = cacheFile();
        QVERIFY(!path.isEmpty());

        // A content change yields a distinct content-addressed key, so the
        // previous entry is never reused; it becomes an orphan for now.
        QFile source(m_documentPath);
        QVERIFY(source.open(QIODevice::Append));
        QCOMPARE(source.write(" changed"), qint64(8));
        source.close();
        QVERIFY(!::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings));
        QVERIFY(QFile::exists(path));

        // A malformed entry at the current key is discarded and removed.
        clearCacheFiles();
        QVERIFY(
            ::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, QByteArray("accelerator")));
        const QString corrupt = cacheFile();
        QVERIFY(!corrupt.isEmpty());
        QFile file(corrupt);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("bad"), qint64(3));
        file.close();
        QVERIFY(!::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings));
        QVERIFY(!QFile::exists(corrupt));
    }

    void sameSizeContentChangeInvalidates()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;

        QFile source(m_documentPath);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write("AAAAAAAAAAA"), qint64(11));
        source.close();
        QVERIFY(
            ::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, QByteArray("accelerator")));
        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings));

        // Same length and same (restored) modification time, different bytes:
        // the bounded content probe must still produce a different key.
        const QDateTime modified = QFileInfo(m_documentPath).lastModified();
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write("BBBBBBBBBBB"), qint64(11));
        QVERIFY(source.setFileTime(modified, QFileDevice::FileModificationTime));
        source.close();

        QVERIFY(!::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings));
    }

    void identicalContentRewriteIsReused()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;
        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, QByteArray("stable")));

        QFile source(m_documentPath);
        QVERIFY(source.open(QIODevice::ReadWrite));
        const QByteArray original = source.readAll();
        QVERIFY(!original.isEmpty());
        QVERIFY(source.seek(0));
        QCOMPARE(source.write(original), qint64(original.size()));
        source.close();

        const auto cached = ::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings);
        QVERIFY(cached);
        QVERIFY(cached->accelerator);
        QCOMPARE(*cached->accelerator, QByteArray("stable"));
    }

    void rejectsOversizedPayload()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;
        QVERIFY(!::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(
            m_documentPath, settings, QByteArray(512 * 1024 + 1, 'x')));
    }
};

QTEST_GUILESS_MAIN(TestEpubCache)

#include "test_epubcache.moc"
