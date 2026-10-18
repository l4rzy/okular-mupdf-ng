// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QTest>
#include <QThread>

#include <atomic>

#include "generator/placeholder.hpp"

using ::Mu::Generator::Placeholder;

class TestGeneratorPlaceholder : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void activationRaisesFlagAndPublishesMessage()
    {
        Placeholder placeholder;
        QVERIFY(!placeholder.isActive());
        QVERIFY(placeholder.message().isEmpty());

        placeholder.activate(QStringLiteral("gate message"), Placeholder::Reason::SandboxGate);
        QVERIFY(placeholder.isActive());
        QCOMPARE(placeholder.reason(), Placeholder::Reason::SandboxGate);
        QCOMPARE(placeholder.message(), QStringLiteral("gate message"));

        // Re-activation of the same state updates the message.
        placeholder.activate(QStringLiteral("other"), Placeholder::Reason::SandboxGate);
        QCOMPARE(placeholder.message(), QStringLiteral("other"));
    }

    void deactivateRequestsReopenOncePerCycle()
    {
        Placeholder placeholder;

        // Inactive: no reopen request.
        QVERIFY(!placeholder.deactivate());

        placeholder.activate(QStringLiteral("m"), Placeholder::Reason::SandboxGate);
        QVERIFY(placeholder.deactivate());
        QVERIFY(placeholder.isActive());

        // Repeated deactivation does not request another reopen.
        QVERIFY(!placeholder.deactivate());

        // After the reopen completes (reset), a new gate cycle must be able
        // to request a reopen again.
        placeholder.reset();
        placeholder.activate(QStringLiteral("m"), Placeholder::Reason::SandboxGate);
        QVERIFY(placeholder.deactivate());
        QVERIFY(placeholder.isActive());
    }

    void resetClearsGateButPreservesTerminalState()
    {
        Placeholder placeholder;

        placeholder.activate(QStringLiteral("m"), Placeholder::Reason::SandboxGate);
        placeholder.reset();
        QVERIFY(!placeholder.isActive());
        QVERIFY(placeholder.message().isEmpty());

        placeholder.activate(QStringLiteral("terminal"), Placeholder::Reason::WorkerUnavailable);
        placeholder.reset();
        QVERIFY(placeholder.isActive());
        QCOMPARE(placeholder.message(), QStringLiteral("terminal"));
    }

    void terminalStateIsStickyAndCannotBeDowngraded()
    {
        Placeholder placeholder;

        placeholder.activate(QStringLiteral("gate"), Placeholder::Reason::SandboxGate);
        // Escalation to terminal replaces the reason and message.
        placeholder.activate(QStringLiteral("terminal"), Placeholder::Reason::WorkerUnavailable);
        QCOMPARE(placeholder.reason(), Placeholder::Reason::WorkerUnavailable);
        QCOMPARE(placeholder.message(), QStringLiteral("terminal"));

        // Deactivation cannot restore a terminal placeholder.
        QVERIFY(!placeholder.deactivate());

        // Later gate activations are ignored.
        placeholder.activate(QStringLiteral("gate"), Placeholder::Reason::SandboxGate);
        QCOMPARE(placeholder.reason(), Placeholder::Reason::WorkerUnavailable);
        QCOMPARE(placeholder.message(), QStringLiteral("terminal"));
    }

    void readerThreadObservesCompleteMessage()
    {
        Placeholder placeholder;
        QVERIFY(!placeholder.isActive());

        std::atomic<bool> stop = false;
        std::atomic<bool> readerReady = false;
        std::atomic<int> observations = 0;
        std::atomic<int> stale = 0;
        std::atomic<int> torn = 0;
        std::atomic<bool> sawInactive = false;
        const QString expected = QStringLiteral("single atomic publication");
        QThread* reader = QThread::create([&] {
            readerReady.store(true);
            while (!stop.load()) {
                if (!placeholder.isActive()) {
                    sawInactive = true;
                    continue;
                }
                // The safety contract is that a reader never observes a torn
                // string. Observing the empty state across a reset() between
                // the two loads is the benign race the generator already
                // tolerates between isActive() and image().
                const QString text = placeholder.message();
                if (text == expected)
                    ++observations;
                else if (text.isEmpty())
                    ++stale;
                else
                    ++torn;
            }
        });
        reader->start();
        for (int spin = 0; spin < 10000 && !readerReady.load(); ++spin)
            QThread::yieldCurrentThread();
        QVERIFY(readerReady.load());

        // Pace the toggles so the reader reliably samples both states.
        for (int i = 0; i < 100; ++i) {
            placeholder.activate(expected, Placeholder::Reason::SandboxGate);
            QThread::msleep(1);
            placeholder.reset();
            QThread::msleep(1);
        }
        // Let the reader observe the final inactive state before stopping.
        for (int i = 0; i < 1000 && (observations.load() == 0 || !sawInactive.load()); ++i)
            QThread::msleep(1);
        stop.store(true);
        QVERIFY(reader->wait(30000));
        delete reader;

        QVERIFY(sawInactive.load());
        QCOMPARE(torn.load(), 0);
        QVERIFY(observations.load() > 0);
    }

    void imageRendersCardAtRequestedDimensions()
    {
        Placeholder placeholder;

        // Invalid dimensions render nothing.
        QVERIFY(placeholder.image(0, 100).isNull());
        QVERIFY(placeholder.image(100, 0).isNull());

        placeholder.activate(QStringLiteral("render me"), Placeholder::Reason::SandboxGate);
        const QImage img = placeholder.image(320, 240);
        QCOMPARE(img.width(), 320);
        QCOMPARE(img.height(), 240);
        QCOMPARE(img.format(), QImage::Format_ARGB32_Premultiplied);

        // An empty message still renders the card.
        placeholder.reset();
        placeholder.activate(QString(), Placeholder::Reason::WorkerUnavailable);
        QVERIFY(!placeholder.image(320, 240).isNull());
    }

    void imageHandlesSmallAndTranslatedCards_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("thumbnail") << QSize(80, 60);
        QTest::newRow("logo-only") << QSize(160, 120);
        QTest::newRow("compact") << QSize(320, 240);
        QTest::newRow("page") << QSize(595, 842);
    }

    void imageHandlesSmallAndTranslatedCards()
    {
        QFETCH(QSize, size);
        Placeholder placeholder;
        placeholder.activate(QStringLiteral("Hộp cát của worker chưa được gia cố đầy đủ.\n"
                                            "Để mở tài liệu này, hãy chuyển chế độ Thực thi hộp cát sang Nới lỏng. "
                                            "Thao tác này sẽ giảm mức bảo vệ.\nLỗi: Không rõ"),
                             Placeholder::Reason::SandboxGate);
        const auto image = placeholder.image(size.width(), size.height());
        QCOMPARE(image.size(), size);
        QCOMPARE(image.format(), QImage::Format_ARGB32_Premultiplied);
    }

    void imagePreservesWrappedHeading()
    {
        const auto originalFont = QGuiApplication::font();
        auto wideFont = originalFont;
        // Force wrapping independently of installed translations and system fonts.
        wideFont.setLetterSpacing(QFont::PercentageSpacing, 200);
        QGuiApplication::setFont(wideFont);
        Placeholder placeholder;
        placeholder.activate(QStringLiteral("message"), Placeholder::Reason::SandboxGate);
        const auto image = placeholder.image(240, 180);
        QGuiApplication::setFont(originalFont);

        int firstRow = image.height(), lastRow = -1;
        for (int y = 0; y < image.height(); ++y)
            for (int x = image.width() / 3; x < image.width(); ++x)
                if (image.pixelColor(x, y) == QColor(139, 86, 17)) {
                    firstRow = qMin(firstRow, y);
                    lastRow = qMax(lastRow, y);
                }
        wideFont.setPixelSize(16);
        wideFont.setBold(true);
        QVERIFY(lastRow - firstRow > QFontMetrics(wideFont).height());
    }
};

QTEST_MAIN(TestGeneratorPlaceholder)

#include "test_placeholder.moc"
