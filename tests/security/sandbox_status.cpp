// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <sstream>

#include "tools/cli/sandbox_report.hpp"

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

    void printsIsolation_data()
    {
        QTest::addColumn<SandboxStatus>("status");
        QTest::addColumn<QString>("expected");
        QTest::newRow("default") << SandboxStatus { } << QStringLiteral("unknown");
        QTest::newRow("empty-protections")
            << SandboxStatus { "linux", "profile", { }, { } } << QStringLiteral("unknown");
        for (const auto* platform : { "linux", "freebsd", "future-os" }) {
            SandboxStatus status {
                platform,
                "profile.v1",
                { { "os.required", ProtectionCategory::Process, ProtectionState::Enforced, true, { }, { } },
                  { "os.optional", ProtectionCategory::Memory, ProtectionState::Unavailable, false, { }, { } } },
                { }
            };
            QTest::newRow(platform) << status << QStringLiteral("full");
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
                << status << (state == ProtectionState::Unknown ? QStringLiteral("unknown") : QStringLiteral("none"));
            status.protections.back().required = true;
            QTest::newRow((std::string(protectionStateName(state)) + "-partial").c_str())
                << status << QStringLiteral("degraded");
        }
        SandboxStatus status {
            "freebsd",
            "profile.v1",
            { { "freebsd.capsicum", ProtectionCategory::Process, ProtectionState::Enforced, false, { }, { } } },
            { }
        };
        QTest::newRow("optional-only") << status << QStringLiteral("none");
        status.protections.front().required = true;
        status.platform.clear();
        QTest::newRow("missing-platform") << status << QStringLiteral("unknown");
        status.platform = "freebsd";
        status.profile.clear();
        QTest::newRow("missing-profile") << status << QStringLiteral("unknown");
        status.profile = "profile.v1";
        status.protections.front().id.clear();
        QTest::newRow("missing-protection-id") << status << QStringLiteral("degraded");
    }

    void printsIsolation()
    {
        QFETCH(SandboxStatus, status);
        QFETCH(QString, expected);

        std::ostringstream human;
        printSandboxStatusHuman(human, status);
        QVERIFY(human.str().contains(" isolation=" + expected.toStdString() + "\n"));

        std::ostringstream json;
        printSandboxStatusJson(json, status);
        const auto report = QJsonDocument::fromJson(QByteArray::fromStdString(json.str())).object();
        QCOMPARE(report.value("isolation").toString(), expected);
        QCOMPARE(report.value("policySatisfied").toBool(), expected == QStringLiteral("full"));
    }

    void printsProtectionLabels_data()
    {
        QTest::addColumn<ProtectionCategory>("category");
        QTest::addColumn<ProtectionState>("state");
        QTest::addColumn<QString>("expectedState");

        QTest::newRow("filesystem") << ProtectionCategory::Filesystem << ProtectionState::Enforced
                                    << QStringLiteral("enforced");
        QTest::newRow("network") << ProtectionCategory::Network << ProtectionState::Unavailable
                                 << QStringLiteral("unavailable");
        QTest::newRow("syscall") << ProtectionCategory::Syscall << ProtectionState::Disabled
                                 << QStringLiteral("disabled");
        QTest::newRow("process") << ProtectionCategory::Process << ProtectionState::Failed << QStringLiteral("failed");
        QTest::newRow("resources") << ProtectionCategory::Resources << ProtectionState::Unknown
                                   << QStringLiteral("unknown");
        QTest::newRow("memory") << ProtectionCategory::Memory << ProtectionState::Enforced
                                << QStringLiteral("enforced");
        QTest::newRow("transport") << ProtectionCategory::Transport << ProtectionState::Enforced
                                   << QStringLiteral("enforced");
        QTest::newRow("other") << ProtectionCategory::Other << ProtectionState::Enforced << QStringLiteral("enforced");
    }

    void printsProtectionLabels()
    {
        QFETCH(ProtectionCategory, category);
        QFETCH(ProtectionState, state);
        QFETCH(QString, expectedState);
        const SandboxStatus status { "linux", "profile", { { "os.required", category, state, true, { }, { } } }, { } };

        std::ostringstream output;
        printSandboxStatusJson(output, status);
        const auto report = QJsonDocument::fromJson(QByteArray::fromStdString(output.str())).object();
        const auto protection = report.value("protections").toArray().at(0).toObject();
        QCOMPARE(protection.value("category").toString(), QString::fromLatin1(QTest::currentDataTag()));
        QCOMPARE(protection.value("state").toString(), expectedState);
    }

    void escapesJsonDiagnostics()
    {
        const SandboxStatus status { "linux", "profile", { }, { "quoted \"text\"\nbackslash \\", "\t\r\x01" } };
        std::ostringstream output;
        printSandboxStatusJson(output, status);
        const auto report = QJsonDocument::fromJson(QByteArray::fromStdString(output.str()));
        QVERIFY(report.isObject());
        const auto diagnostics = report.object().value("diagnostics").toArray();
        QCOMPARE(diagnostics.size(), 2);
        QCOMPARE(diagnostics.at(0).toString(), QStringLiteral("quoted \"text\"\nbackslash \\"));
        QCOMPARE(diagnostics.at(1).toString(), QStringLiteral("\t\r\x01"));
    }
};

QTEST_GUILESS_MAIN(TestSandboxStatus)
#include "sandbox_status.moc"
