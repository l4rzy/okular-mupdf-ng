// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "shared/protocol/zpp_codec.hpp"

using namespace Mu::Model;

class TestSandboxStatus : public QObject {
    Q_OBJECT

private slots:

    void evaluatesPolicy_data()
    {
        QTest::addColumn<SandboxStatus>("status");
        QTest::addColumn<IsolationLevel>("isolation");
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
        QFETCH(IsolationLevel, isolation);
        QCOMPARE(status.isolation(), isolation);
        QCOMPARE(status.isFullyHardened(), isolation == IsolationLevel::Full);
        // Missing platform/profile metadata leaves the overall isolation unknown.
        if (!status.platform.empty() && !status.profile.empty())
            QCOMPARE(status.isPartiallyActive(), isolation == IsolationLevel::Degraded);
    }

    void roundTripsReport()
    {
        SandboxStatus status { "future-os",
                               "profile.v3",
                               { { "future-os.control",
                                   ProtectionCategory::Transport,
                                   ProtectionState::Enforced,
                                   true,
                                   "3",
                                   "control diagnostic" },
                                 { "future-os.optional",
                                   ProtectionCategory::Other,
                                   ProtectionState::Unavailable,
                                   false,
                                   { },
                                   "optional diagnostic" } },
                               { "first diagnostic", "second diagnostic" } };
        ResponseMessage message { 42, PingResponse { "compat", 123, status, "engine", false }, { } };
        std::string error;
        auto bytes = Mu::IPC::ZppCodec::encode(message, &error);
        QVERIFY2(bytes.has_value(), error.c_str());
        ResponseMessage decoded;
        QVERIFY2(Mu::IPC::ZppCodec::decode(*bytes, &decoded, &error), error.c_str());
        const auto* ping = std::get_if<PingResponse>(&decoded.payload);
        QVERIFY(ping);
        const auto& report = ping->sandbox;
        QCOMPARE(report.platform, status.platform);
        QCOMPARE(report.profile, status.profile);
        QCOMPARE(report.diagnostics, status.diagnostics);
        QCOMPARE(report.protections.size(), status.protections.size());
        for (std::size_t i = 0; i < status.protections.size(); ++i) {
            const auto& actual = report.protections[i];
            const auto& expected = status.protections[i];
            QCOMPARE(actual.id, expected.id);
            QCOMPARE(actual.category, expected.category);
            QCOMPARE(actual.state, expected.state);
            QCOMPARE(actual.required, expected.required);
            QCOMPARE(actual.version, expected.version);
            QCOMPARE(actual.diagnostic, expected.diagnostic);
        }
        QVERIFY(report.isFullyHardened());
        // Version 18 used the incompatible Linux-specific sandbox layout.
        (*bytes)[4] = std::byte { 18 };
        QVERIFY(!Mu::IPC::ZppCodec::decode(*bytes, &decoded, &error));
    }
};

QTEST_GUILESS_MAIN(TestSandboxStatus)
#include "sandbox_status.moc"
