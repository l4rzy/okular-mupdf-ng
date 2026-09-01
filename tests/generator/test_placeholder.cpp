// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QImage>
#include <QTest>
#include <QThread>

#include <atomic>

#include "generator/placeholder.hpp"

using ::Mu::Generator::Placeholder;
using ::Mu::Model::SandboxStatus;
namespace Gate = ::Mu::Generator::SandboxGate;

namespace {

SandboxStatus unhardenedStatus()
{
    return SandboxStatus { };
}

} // namespace

class TestGeneratorPlaceholder : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void withheldPageDimensions()
    {
        // At 72 dpi the page keeps its exact A4 point dimensions.
        auto* page = Gate::withheldPage(72.0, 72.0);
        QCOMPARE(page->number(), 0);
        QCOMPARE(page->width(), 595.0);
        QCOMPARE(page->height(), 842.0);
        QCOMPARE(page->rotation(), Okular::Rotation0);
        delete page;

        // Higher dpi scales the pixel dimensions proportionally.
        page = Gate::withheldPage(96.0, 96.0);
        QVERIFY(qFuzzyCompare(page->width(), 595.0 * 96.0 / 72.0));
        QVERIFY(qFuzzyCompare(page->height(), 842.0 * 96.0 / 72.0));
        delete page;
    }

    void guidanceMessageEmbedsReason()
    {
        SandboxStatus status = unhardenedStatus();
        status.reason = "landlock unavailable";
        const QString message = Gate::guidanceMessage(status);
        QVERIFY(!message.isEmpty());
        QVERIFY(message.contains(QStringLiteral("(landlock unavailable)")));

        // An empty reason must not leave dangling parentheses.
        status.reason.clear();
        const QString bare = Gate::guidanceMessage(status);
        QVERIFY(!bare.isEmpty());
        QVERIFY(!bare.contains(QStringLiteral("()")));
    }

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
        while (!readerReady.load()) { }

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
        reader->wait();
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
};

QTEST_MAIN(TestGeneratorPlaceholder)

#include "test_placeholder.moc"
