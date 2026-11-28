// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <sstream>

#include "shared/sandbox_report.hpp"

using namespace paperbox::model;
using namespace Mu::Cli;

class TestSandboxStatus : public QObject {
    Q_OBJECT

private slots:

    void printsProtectionDiagnostic_data()
    {
        QTest::addColumn<QString>("diagnostic");
        QTest::newRow("empty") << QString();
        QTest::newRow("inherited") << QStringLiteral("Namespaces inherited from Bubblewrap launcher");
    }

    void printsProtectionDiagnostic()
    {
        QFETCH(QString, diagnostic);
        const SandboxStatus status {
            "linux",
            "profile.v1",
            { { "linux.namespaces",
                ProtectionCategory::Process,
                ProtectionState::Enforced,
                true,
                { },
                diagnostic.toStdString() },
              { "linux.resource_limits", ProtectionCategory::Resources, ProtectionState::Enforced, true, { }, { } } },
            { "general diagnostic" }
        };
        std::ostringstream output;
        printSandboxStatusHuman(output, status);
        const std::string expectedDiagnostic = diagnostic.isEmpty() ? "" : "    " + diagnostic.toStdString() + "\n";
        QCOMPARE(output.str(),
                 "Sandbox: linux profile=profile.v1 isolation=full\n"
                 "  linux.namespaces: enforced required=1 version=\n"
                     + expectedDiagnostic
                     + "  linux.resource_limits: enforced required=1 version=\n"
                       "  general diagnostic\n");
    }

    void evaluatesPolicy_data()
    {
        QTest::addColumn<SandboxStatus>("status");
        QTest::addColumn<IsolationLevel>("expected");
        QTest::newRow("default") << SandboxStatus { } << IsolationLevel::Unknown;
        QTest::newRow("empty-protections") << SandboxStatus { "linux", "profile", { }, { } } << IsolationLevel::Unknown;
        for (const auto* platform : { "linux", "freebsd", "future-os" }) {
            SandboxStatus status {
                platform,
                "profile.v1",
                { { "os.required", ProtectionCategory::Process, ProtectionState::Enforced, true, { }, { } },
                  { "os.optional", ProtectionCategory::Memory, ProtectionState::Unavailable, false, { }, { } } },
                { }
            };
            QTest::newRow(platform) << status << IsolationLevel::Full;
        }
        for (const auto state : { ProtectionState::Unknown,
                                  ProtectionState::Unavailable,
                                  ProtectionState::Disabled,
                                  ProtectionState::Failed }) {
            SandboxStatus status {
                "future-os",
                "profile.v1",
                { { "os.required", ProtectionCategory::Process, state, true, { }, { } },
                  { "os.optional", ProtectionCategory::Memory, ProtectionState::Enforced, false, { }, { } } },
                { }
            };
            QTest::newRow(protectionStateName(state).data())
                << status << (state == ProtectionState::Unknown ? IsolationLevel::Unknown : IsolationLevel::None);
            status.protections.back().required = true;
            QTest::newRow((std::string(protectionStateName(state)) + "-partial").c_str())
                << status << IsolationLevel::Degraded;
        }
        SandboxStatus status {
            "freebsd",
            "profile.v1",
            { { "freebsd.capsicum", ProtectionCategory::Process, ProtectionState::Enforced, false, { }, { } } },
            { }
        };
        QTest::newRow("optional-only") << status << IsolationLevel::None;
        status.protections.front().required = true;
        status.platform.clear();
        QTest::newRow("missing-platform") << status << IsolationLevel::Unknown;
        status.platform = "freebsd";
        status.profile.clear();
        QTest::newRow("missing-profile") << status << IsolationLevel::Unknown;
        status.profile = "profile.v1";
        status.protections.front().id.clear();
        QTest::newRow("missing-protection-id") << status << IsolationLevel::Degraded;
    }

    void evaluatesPolicy()
    {
        QFETCH(SandboxStatus, status);
        QFETCH(IsolationLevel, expected);
        QCOMPARE(isolation(status), expected);
        QCOMPARE(status.isFullyHardened(), expected == IsolationLevel::Full);
        // Missing platform/profile metadata leaves the overall isolation unknown.
        if (!status.platform.empty() && !status.profile.empty())
            QCOMPARE(status.isPartiallyActive(), expected == IsolationLevel::Degraded);
    }

    void escapesJsonDiagnostics()
    {
        const SandboxStatus status { "linux", "profile", { }, { "quoted \"text\"\nbackslash \\" } };
        std::ostringstream output;
        printSandboxStatusJson(output, status);
        const auto report = QJsonDocument::fromJson(QByteArray::fromStdString(output.str()));
        QVERIFY(report.isObject());
        QCOMPARE(report.object().value("diagnostics").toArray().at(0).toString(),
                 QStringLiteral("quoted \"text\"\nbackslash \\"));
    }
};

QTEST_GUILESS_MAIN(TestSandboxStatus)
#include "sandbox_status.moc"
