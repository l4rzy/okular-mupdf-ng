// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFontMetrics>
#include <QImage>
#include <QTest>

#include <algorithm>

#include <QFont>

#include "generator/config/signature_preview.hpp"
#include "plugin/util/signing_timestamp.hpp"

using Mu::Generator::Config::buildSignaturePreview;
using Mu::Generator::Config::renderSignaturePreview;
using Mu::Generator::Config::SignaturePreview;

QImage renderLines(const QStringList& lines)
{
    return renderSignaturePreview({ { }, lines }, QFont(), 1.0);
}

class TestGeneratorSignaturePreview : public QObject {
    Q_OBJECT

private slots:

    void buildsSignaturePreview()
    {
        const QDateTime now = QDateTime::fromSecsSinceEpoch(1757248440);

        const auto complete = buildSignaturePreview(false, false, now);
        QCOMPARE(complete.leftText, QStringLiteral("Jane Doe"));
        QCOMPARE(complete.rightLines.size(), 5);
        QCOMPARE(complete.rightLines.at(0), QStringLiteral("Digitally signed by Jane Doe"));
        QCOMPARE(complete.rightLines.at(1), QStringLiteral("DN: CN=Jane Doe, O=Example"));
        QCOMPARE(complete.rightLines.at(2), QStringLiteral("Reason: Approved"));
        QCOMPARE(complete.rightLines.at(3), QStringLiteral("Location: Berlin"));
        QCOMPARE(complete.rightLines.at(4),
                 QStringLiteral("Date: ") + Mu::Plugin::Util::SigningTimestamp::displayDate(now.toLocalTime()));

        const auto simple = buildSignaturePreview(true, true, now);
        QVERIFY(simple.leftText.isEmpty());
        QCOMPARE(simple.rightLines.size(), 3);
        QCOMPARE(simple.rightLines.at(0), QStringLiteral("Jane Doe"));
        QCOMPARE(simple.rightLines.at(1), QStringLiteral("Approved"));
        const QDateTime utc = now.toUTC();
        QCOMPARE(simple.rightLines.at(2), Mu::Plugin::Util::SigningTimestamp::displayDate(utc));
        QVERIFY(simple.rightLines.at(2).endsWith(QStringLiteral("UTC")));
    }

    void rendersWhiteBoxWithBorder()
    {
        const QFont font;
        const QStringList lines { QStringLiteral("Jane Doe"),
                                  QStringLiteral("Approved"),
                                  QStringLiteral("Sep 7, 2026 13:14 UTC") };
        const QImage image = renderLines(lines);

        QVERIFY(!image.isNull());
        const QFontMetrics metrics(font);
        int textWidth = 0;
        for (const QString& line : lines)
            textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
        QCOMPARE(image.size(), QSize(textWidth + 22, metrics.lineSpacing() * lines.size() + 22));
        // White document background in the corners, grey 1px border on top.
        QCOMPARE(image.pixelColor(0, 0), QColor(0x9a, 0x9a, 0x9a));
        QCOMPARE(image.pixelColor(5, 5), QColor(Qt::white));
        QCOMPARE(image.pixelColor(image.width() - 1, image.height() - 1), QColor(0x9a, 0x9a, 0x9a));
    }

    void twoPaneLayoutIsWiderThanSinglePane()
    {
        const QFont font;
        const QStringList lines { QStringLiteral("Digitally signed by Jane Doe"),
                                  QStringLiteral("Date: Sep 7, 2026 13:14 UTC") };
        const QImage single = renderLines(lines);
        const QImage twoPane = renderSignaturePreview({ QStringLiteral("Jane Doe"), lines }, font, 1.0);

        QVERIFY(!twoPane.isNull());
        QCOMPARE(twoPane.height(), single.height());
        QVERIFY(twoPane.width() > single.width());
        QCOMPARE(twoPane.pixelColor(0, 0), QColor(0x9a, 0x9a, 0x9a));
        QCOMPARE(twoPane.pixelColor(5, 5), QColor(Qt::white));
    }

    void emptyLinesStillRenderPaddedBox()
    {
        const QImage image = renderLines({ });
        QVERIFY(!image.isNull());
        QCOMPARE(image.pixelColor(5, 5), QColor(Qt::white));
    }
};

QTEST_MAIN(TestGeneratorSignaturePreview)

#include "test_signature_preview.moc"
