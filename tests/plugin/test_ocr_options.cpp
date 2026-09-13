// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "plugin/util/ocr_options.hpp"

namespace OcrOptions = Mu::Plugin::Util;

class TestPluginOcrOptions : public QObject {
    Q_OBJECT

private slots:

    void stripsLanguageSuffix()
    {
        QCOMPARE(OcrOptions::stripLangSuffix(QStringLiteral("eng.traineddata")), QStringLiteral("eng"));
        QCOMPARE(OcrOptions::stripLangSuffix(QStringLiteral("eng")), QStringLiteral("eng"));
        QCOMPARE(OcrOptions::stripLangSuffix(QStringLiteral("deu_300dpi")), QStringLiteral("deu_300dpi"));
        QCOMPARE(OcrOptions::stripLangSuffix(QString()), QString());
    }

    void mapsQualityToDpi()
    {
        // Unknown quality values fall back to Balanced, never the slowest mode.
        QCOMPARE(OcrOptions::qualityToDpi(0), 150.0f);
        QCOMPARE(OcrOptions::qualityToDpi(1), 225.0f);
        QCOMPARE(OcrOptions::qualityToDpi(2), 300.0f);
        QCOMPARE(OcrOptions::qualityToDpi(-1), 225.0f);
        QCOMPARE(OcrOptions::qualityToDpi(99), 225.0f);
    }
};

QTEST_GUILESS_MAIN(TestPluginOcrOptions)

#include "test_ocr_options.moc"
