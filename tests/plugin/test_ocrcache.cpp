// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/cache_root.hpp"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <paperbox/cache.hpp>

class TestLegacyOcrCache : public QObject {
    Q_OBJECT

private slots:

    void readsQtCache_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("storedDpi");
        QTest::newRow("empty-hit") << QString() << 150;
        QTest::newRow("utf16-text") << QStringLiteral("Résumé") << 150;
        QTest::newRow("higher-dpi") << QStringLiteral("Text") << 300;
    }

    void readsQtCache()
    {
        QFETCH(QString, text);
        QFETCH(int, storedDpi);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        qputenv("XDG_CACHE_HOME", QFile::encodeName(directory.path()));

        const QString cacheDir = directory.filePath(QStringLiteral("mupdfng/ocr/document"));
        QVERIFY(QDir().mkpath(cacheDir));
        QFile legacy(QDir(cacheDir).filePath(QStringLiteral("p0_eng_%1dpiv1.bin").arg(storedDpi)));
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        QDataStream header(&legacy);
        header.setVersion(QDataStream::Qt_6_0);
        header << quint32(0x4f435232) << quint32(text.isEmpty() ? 0 : 1);
        if (!text.isEmpty()) {
            QByteArray payload;
            QDataStream record(&payload, QIODevice::WriteOnly);
            record.setVersion(QDataStream::Qt_6_0);
            record << text << 0.1 << 0.2 << 0.8 << 0.9;
            const auto compressed = qCompress(payload, 1);
            QCOMPARE(legacy.write(compressed), compressed.size());
        }
        legacy.close();

        const auto result = paperbox::loadCachedOcr(Mu::Plugin::Caching::cacheRoot(), "document", 0, "eng", 150);
        QVERIFY(result.has_value());
        QCOMPARE(result->size(), text.isEmpty() ? 0u : 1u);
        if (!text.isEmpty()) {
            QCOMPARE(QString::fromStdString(result->front().text), text);
            QCOMPARE(result->front().top, 0.2);
        }
    }
};

QTEST_GUILESS_MAIN(TestLegacyOcrCache)
#include "test_ocrcache.moc"
