// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QTest>

#include <cstdlib>

#include "plugin/util/signature_image.hpp"

namespace SignatureImage = Mu::Plugin::Util::SignatureImage;

class TestSignatureImage : public QObject {
    Q_OBJECT

private slots:

    void rejectsUnreadableSource_data()
    {
        QTest::addColumn<QString>("source");
        QTest::newRow("empty-path") << QString();
        QTest::newRow("missing-file") << QStringLiteral("missing.png");
        QTest::newRow("corrupt-image") << QStringLiteral("corrupt.png");
        QTest::newRow("empty-file") << QStringLiteral("empty.png");
    }

    void rejectsUnreadableSource()
    {
        QFETCH(QString, source);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        if (source == QStringLiteral("corrupt.png") || source == QStringLiteral("empty.png")) {
            QFile file(directory.filePath(source));
            QVERIFY(file.open(QIODevice::WriteOnly));
            if (source == QStringLiteral("corrupt.png"))
                QCOMPARE(file.write("not an image"), qint64(12));
        }
        const QString path = source.isEmpty() ? QString() : directory.filePath(source);
        QVERIFY(SignatureImage::prepareBackgroundImage(path).empty());
    }

    void scalesAndEncodesImage_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<QSize>("sourceSize");
        QTest::addColumn<double>("rectWidth");
        QTest::addColumn<double>("rectHeight");
        QTest::addColumn<double>("pageWidth");
        QTest::addColumn<double>("pageHeight");
        QTest::addColumn<QSize>("expectedSize");
        QTest::addColumn<bool>("transparent");
        QTest::newRow("default-page-and-rectangle")
            << QByteArray("PNG") << QSize(200, 100) << 0.0 << 0.0 << 0.0 << 0.0 << QSize(336, 168) << false;
        QTest::newRow("negative-geometry-fallback")
            << QByteArray("PNG") << QSize(100, 200) << -1.0 << -1.0 << -1.0 << -1.0 << QSize(84, 168) << false;
        QTest::newRow("minimum-bounds") << QByteArray("PNG") << QSize(200, 100) << 0.001 << 0.001 << 100.0 << 100.0
                                        << QSize(64, 32) << false;
        QTest::newRow("maximum-bounds") << QByteArray("PNG") << QSize(300, 200) << 1.0 << 1.0 << 1000.0 << 1000.0
                                        << QSize(384, 256) << false;
        QTest::newRow("portrait-aspect-ratio")
            << QByteArray("PNG") << QSize(100, 200) << 0.5 << 0.5 << 200.0 << 100.0 << QSize(50, 100) << false;
        QTest::newRow("jpeg-to-png") << QByteArray("JPEG") << QSize(200, 100) << 0.5 << 0.5 << 200.0 << 100.0
                                     << QSize(200, 100) << false;
        QTest::newRow("transparent-png") << QByteArray("PNG") << QSize(200, 100) << 0.5 << 0.5 << 200.0 << 100.0
                                         << QSize(200, 100) << true;
    }

    void scalesAndEncodesImage()
    {
        QFETCH(QByteArray, format);
        QFETCH(QSize, sourceSize);
        QFETCH(double, rectWidth);
        QFETCH(double, rectHeight);
        QFETCH(double, pageWidth);
        QFETCH(double, pageHeight);
        QFETCH(QSize, expectedSize);
        QFETCH(bool, transparent);
        if (format == "JPEG" && !QImageWriter::supportedImageFormats().contains("jpeg"))
            QSKIP("The optional JPEG image codec is not installed");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("source.image"));
        QImage source(sourceSize, QImage::Format_ARGB32);
        source.fill(QColor(32, 96, 160, transparent ? 80 : 255));
        QVERIFY(source.save(path, format.constData()));

        const auto result = SignatureImage::prepareBackgroundImage(path, rectWidth, rectHeight, pageWidth, pageHeight);
        QVERIFY(!result.empty());
        const QByteArray bytes(reinterpret_cast<const char*>(result.data()), static_cast<qsizetype>(result.size()));
        QVERIFY(bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
        const QImage decoded = QImage::fromData(bytes, "PNG");
        QVERIFY(!decoded.isNull());
        QCOMPARE(decoded.size(), expectedSize);
        const QColor pixel = decoded.pixelColor(decoded.width() / 2, decoded.height() / 2);
        QCOMPARE(pixel.alpha(), transparent ? 80 : 255);
        // JPEG conversion can round color channels; the image must retain its color.
        QVERIFY(std::abs(pixel.red() - 32) <= 2);
        QVERIFY(std::abs(pixel.green() - 96) <= 2);
        QVERIFY(std::abs(pixel.blue() - 160) <= 2);
    }
};

QTEST_GUILESS_MAIN(TestSignatureImage)

#include "test_signature_image.moc"
