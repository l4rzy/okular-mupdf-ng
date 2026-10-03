// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "plugin/signing_authorization.hpp"

class TestSigningAuthorization : public QObject {
    Q_OBJECT

private slots:

    void authorizeCallback_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("active-request") << 0 << true;
        QTest::newRow("idle-or-unrelated-rpc") << 1 << false;
        QTest::newRow("wrong-request") << 2 << false;
        QTest::newRow("wrong-certificate") << 3 << false;
        QTest::newRow("duplicate") << 4 << false;
        QTest::newRow("after-completion-or-disconnect") << 5 << false;
        QTest::newRow("replay-during-next-request") << 6 << false;
    }

    void authorizeCallback()
    {
        QFETCH(int, scenario);
        QFETCH(bool, accepted);
        Mu::Plugin::SigningAuthorization authorization { 42, "selected" };
        Mu::Model::SignInput input { 42, "42", "selected", { } };
        switch (scenario) {
        case 1:
            authorization = { };
            break;
        case 2:
            input.jobId = 43;
            break;
        case 3:
            input.certificateNickname = "other";
            break;
        case 4:
            QVERIFY(authorization.accept(input));
            break;
        case 5:
            QVERIFY(authorization.accept(input));
            authorization = { };
            break;
        case 6:
            authorization.requestId = 43;
            break;
        }
        QCOMPARE(authorization.accept(input), accepted);
        if (accepted)
            QVERIFY(!authorization.accept(input));
    }
};

QTEST_GUILESS_MAIN(TestSigningAuthorization)
#include "test_signing_authorization.moc"
