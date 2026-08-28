// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDataStream>
#include <QDirIterator>
#include <QFile>
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

    void migratesAcceleratorOnlyV1()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;
        const QByteArray accelerator("legacy accelerator");
        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, accelerator));
        const QString path = cacheFile();
        QVERIFY(!path.isEmpty());

        QFileInfo source(m_documentPath);
        QByteArray legacy;
        QDataStream stream(&legacy, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << quint32(0x45504131) << quint32(1) << source.size() << source.lastModified().toMSecsSinceEpoch()
               << static_cast<quint32>(accelerator.size());
        legacy.append(accelerator);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(legacy), qint64(legacy.size()));
        file.close();

        const auto migrated = ::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings);
        QVERIFY(migrated);
        QVERIFY(migrated->accelerator);
        QCOMPARE(*migrated->accelerator, accelerator);
        QVERIFY(!migrated->outline);

        QVERIFY(::Mu::Plugin::Caching::EPUB::Cache::saveOutline(m_documentPath, settings, { }));
        const auto upgraded = ::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings);
        QVERIFY(upgraded);
        QVERIFY(upgraded->accelerator);
        QVERIFY(upgraded->outline);
        QVERIFY(upgraded->outline->empty());
    }

    void invalidatesSourceChangesAndRemovesCorruption()
    {
        clearCacheFiles();
        ::Mu::Model::DocumentSettings settings;
        QVERIFY(
            ::Mu::Plugin::Caching::EPUB::Cache::saveAccelerator(m_documentPath, settings, QByteArray("accelerator")));
        const QString path = cacheFile();
        QVERIFY(!path.isEmpty());

        QFile source(m_documentPath);
        QVERIFY(source.open(QIODevice::Append));
        QCOMPARE(source.write(" changed"), qint64(8));
        source.close();
        QVERIFY(!::Mu::Plugin::Caching::EPUB::Cache::load(m_documentPath, settings));
        QVERIFY(!QFile::exists(path));

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
