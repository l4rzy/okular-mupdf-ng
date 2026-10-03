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
        QTest::newRow("wrong-request-or-replay") << 2 << false;
        QTest::newRow("wrong-certificate") << 3 << false;
        QTest::newRow("after-completion-or-disconnect") << 5 << false;
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
        case 5:
            QVERIFY(authorization.accept(input));
            authorization = { };
            break;
        }
        QCOMPARE(authorization.accept(input), accepted);
        if (accepted)
            QVERIFY(!authorization.accept(input));
    }
};

QTEST_GUILESS_MAIN(TestSigningAuthorization)
#include "test_signing_authorization.moc"
