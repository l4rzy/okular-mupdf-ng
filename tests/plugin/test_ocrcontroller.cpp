// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "plugin/caching/cache_file.hpp"

#include "plugin/ocr/controller.hpp"
#include "plugin/ocr/policy.hpp"

using Mu::Plugin::OCR::Controller;
using Mu::Plugin::OCR::VisiblePage;

class TestPluginOcrController : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void ignoresInvalidVisibilityAndDiscardedObservations_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::newRow("no-visible-pages") << 0;
        QTest::newRow("negative-page") << 1;
        QTest::newRow("zero-visible-area") << 2;
        QTest::newRow("observation-queued-before-reset") << 3;
    }

    void ignoresInvalidVisibilityAndDiscardedObservations()
    {
        QFETCH(int, scenario);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        Mu::Plugin::Caching::setRootForTesting(root.path());
        const auto restoreCacheRoot = qScopeGuard([] { Mu::Plugin::Caching::clearRootForTesting(); });
        Mu::Plugin::WorkerClient backend;
        QVERIFY(backend.start(QStringLiteral(WORKER_BUILD_PATH)));
        QList<Mu::Plugin::WorkerClient::PageInfo> pages;
        QCOMPARE(backend.open(QStringLiteral(TEST_PDF_PATH), { }, pages), Mu::Model::OpenStatus::Success);
        backend.commitSessionReady();
        QVERIFY(backend.operational());
        Mu::Plugin::OCR::Config config;
        config.documentHash = QString::fromStdString(backend.getDocumentInfo({ "hash" }).values.at("hash"));
        config.language = QStringLiteral("eng");
        config.pageCount = 1;
        config.force = true;
        config.debounceMs = 5;
        const auto key =
            Mu::Plugin::Caching::OCR::Cache::normalizeKey(config.documentHash, config.language, config.dpi);
        QVERIFY(key);
        Mu::Plugin::Caching::OCR::Cache::save(*key, 0, { { QStringLiteral("cached"), 0.1, 0.1, 0.9, 0.9 } });
        QVERIFY(Mu::Plugin::Caching::OCR::Cache::load(*key, 0).present);
        Controller controller(&backend);
        QSignalSpy completed(&controller, &Controller::completed);
        QSignalSpy started(&controller, &Controller::started);
        QSignalSpy failed(&controller, &Controller::failed);
        const QList<VisiblePage> invalid[] = { { }, { { -1, 1.0 } }, { { 0, 0.0 } }, { { 0, 1.0 } } };
        controller.observeVisiblePages(invalid[scenario], config);
        if (scenario == 3)
            controller.reset();
        // Process queued observations and wait beyond the configured debounce.
        QTest::qWait(50);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(started.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!controller.takeReady(0));

        // Positive control: the same backend and cache must deliver a valid focus.
        controller.observeVisiblePages({ { 0, 1.0 } }, config);
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(completed.front().at(0).toInt(), 0);
        QCOMPARE(completed.front().at(2).value<Controller::CompletionSource>(),
                 Controller::CompletionSource::CacheLoaded);
        const auto ready = controller.takeReady(0);
        QVERIFY(ready);
        QCOMPARE(ready->size(), 1);
        QCOMPARE(ready->front().ch, QStringLiteral("cached"));
        QVERIFY(!controller.takeReady(0));
        controller.reset();
        controller.observeVisiblePages({ { 0, 1.0 } }, config);
        QTRY_COMPARE(completed.count(), 2);
        controller.reset();
        QVERIFY(!controller.takeReady(0));
        QCOMPARE(started.count(), 0);
        QCOMPARE(failed.count(), 0);
    }

    void focusPolicyTracksDirectionAndWindow()
    {
        ::Mu::Plugin::OCR::FocusPolicy focus;
        QVERIFY(focus.noteFocus(10, 100));
        QVERIFY(!focus.noteFocus(10, 100));
        QCOMPARE(focus.prefetchWindow(), QList<int>({ 10, 11, 9 }));

        QVERIFY(focus.noteFocus(52, 100));
        QCOMPARE(focus.prefetchWindow(), QList<int>({ 52, 53, 51 }));

        QVERIFY(focus.noteFocus(10, 100));
        QCOMPARE(focus.prefetchWindow(), QList<int>({ 10, 9, 11 }));
    }

    void focusPolicyHandlesDocumentEdges()
    {
        ::Mu::Plugin::OCR::FocusPolicy focus;
        focus.noteFocus(0, 2);
        QCOMPARE(focus.prefetchWindow(), QList<int>({ 0, 1 }));
        focus.noteFocus(1, 2);
        QCOMPARE(focus.prefetchWindow(), QList<int>({ 1, 0 }));

        focus.reset();
        focus.noteFocus(0, 1);
        QCOMPARE(focus.prefetchWindow(), QList<int>({ 0 }));
    }

    void pageQueuePrioritizesByFocusAndEvictsStale()
    {
        ::Mu::Plugin::OCR::PageQueue queue;
        queue.refresh(50, { 50, 51, 49 }, 2, 5);
        QCOMPARE(queue.pages(), QList<int>({ 50, 51, 49 }));

        // A far jump discards queued work that is now outside the radius.
        queue.refresh(55, { 55, 56, 54 }, 2, 5);
        QCOMPARE(queue.pages(), QList<int>({ 55, 56, 54 }));
        QVERIFY(!queue.contains(50));
        QVERIFY(queue.contains(54));
        QVERIFY(queue.contains(56));
    }

    void pageQueueTakeNextRevalidatesAgainstFocus()
    {
        ::Mu::Plugin::OCR::PageQueue queue;
        queue.refresh(10, { 10, 11, 9 }, 2, 5);

        // Focus moved far away before dispatch: the stale head is dropped.
        QVERIFY(!queue.takeNext(30, 2).has_value());
        QVERIFY(queue.isEmpty());

        queue.refresh(30, { 30, 31, 29 }, 2, 5);
        const auto next = queue.takeNext(30, 2);
        QVERIFY(next.has_value());
        QCOMPARE(*next, 30);
        QCOMPARE(queue.pages(), QList<int>({ 31, 29 }));
    }

    void pageQueuePushFrontKeepsRetry()
    {
        ::Mu::Plugin::OCR::PageQueue queue;
        queue.refresh(40, { 40, 41, 39 }, 2, 5);
        const auto next = queue.takeNext(40, 2);
        QVERIFY(next.has_value());
        QCOMPARE(*next, 40);

        queue.pushFront(40, 40, 2);
        QCOMPARE(queue.pages().constFirst(), 40);

        // A retried page outside the window is not resurrected.
        queue.pushFront(99, 40, 2);
        QVERIFY(!queue.contains(99));
    }

    void retryPolicyBoundsAttempts()
    {
        ::Mu::Plugin::OCR::RetryPolicy retry(3);
        QVERIFY(!retry.exhausted(7));
        QVERIFY(retry.onFailure(7));
        QVERIFY(retry.onFailure(7));
        QVERIFY(!retry.onFailure(7));
        QVERIFY(retry.exhausted(7));

        retry.clear(7);
        QVERIFY(!retry.exhausted(7));
        QVERIFY(retry.onFailure(7));
    }

    void dominantPageHysteresisTable()
    {
        const struct {
            QList<VisiblePage> pages;
            int previousPage;
            int expected;
        } cases[] = {
            // No valid entries: no dominant page.
            { { }, -1, -1 },
            { { { -1, 10.0 }, { 2, 0.0 } }, -1, -1 },
            // Largest visible area wins.
            { { { 1, 5.0 }, { 2, 9.0 } }, -1, 2 },
            { { { 10, 0.7 * 600 * 800 }, { 11, 0.6 * 1000 * 1000 } }, -1, 11 },
            // Entries for the same page are aggregated before comparing.
            { { { 1, 4.0 }, { 1, 4.0 }, { 2, 7.0 } }, -1, 1 },
            // A tie is resolved toward the previous focus page.
            { { { 1, 5.0 }, { 2, 5.0 } }, 2, 2 },
            { { { 1, 5.0 }, { 2, 5.0 } }, 1, 1 },
            // A tie without a previous focus resolves toward the lower page number.
            { { { 2, 5.0 }, { 1, 5.0 } }, -1, 1 },
        };

        for (const auto& testCase : cases)
            QCOMPARE(Mu::Plugin::OCR::dominantPage(testCase.pages, testCase.previousPage), testCase.expected);
    }

    void shouldTriggerTable()
    {
        const struct {
            bool force;
            bool autoTrigger;
            unsigned threshold;
            std::size_t existingTextBoxCount;
            bool expected;
        } cases[] = {
            { false, false, 20, 0, false },
            { false, true, 20, 19, true },
            { false, true, 20, 20, false },
            { true, false, 20, 100, true },
        };

        for (const auto& testCase : cases) {
            QCOMPARE(Controller::shouldTrigger(
                         testCase.force, testCase.autoTrigger, testCase.threshold, testCase.existingTextBoxCount),
                     testCase.expected);
        }
    }

    void debounceDelayDoesNotInvalidateConfig()
    {
        // Retuning the settle delay must not discard queued work, so it is
        // excluded from Config equality; scheduling inputs still compare.
        Mu::Plugin::OCR::Config base;
        auto retuned = base;
        retuned.debounceMs = 100;
        QVERIFY(base == retuned);
        auto rescaled = base;
        rescaled.dpi = 300;
        QVERIFY(!(base == rescaled));
    }
};

QTEST_MAIN(TestPluginOcrController)

#include "test_ocrcontroller.moc"
