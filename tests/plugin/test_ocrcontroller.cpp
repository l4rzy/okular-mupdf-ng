// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "plugin/ocr/controller.hpp"
#include "plugin/ocr/scheduler.hpp"

using Mu::Plugin::OCR::Controller;
using Mu::Plugin::OCR::VisiblePage;

class TestPluginOcrController : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void observeVisiblePagesWithoutDominantPageIsNoop()
    {
        Mu::Plugin::WorkerClient backend;
        Controller controller(&backend);

        // Fresh and reset controllers hold no results for any job id.
        QVERIFY(!controller.takeReady(0).has_value());
        QVERIFY(!controller.takeReady(7).has_value());
        QVERIFY(!controller.takeReady(-1).has_value());
        controller.reset();
        QVERIFY(!controller.takeReady(0).has_value());
        controller.reset();
        QVERIFY(!controller.takeReady(0).has_value());

        // No visible pages and invalid entries must not schedule anything;
        // the controller stays idle and no result appears.
        controller.observeVisiblePages({ }, { });
        controller.observeVisiblePages({ { -1, 10.0 }, { 3, -1.0 } }, { });
        QVERIFY(!controller.takeReady(3).has_value());
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
};

QTEST_MAIN(TestPluginOcrController)

#include "test_ocrcontroller.moc"
