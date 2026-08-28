// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QGuiApplication>
#include <QTest>

#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

int runTestWorkerDocument(int argc, char** argv);
int runTestWorkerPage(int argc, char** argv);
int runTestWorkerSignature(int argc, char** argv);
int runTestPluginSignature(int argc, char** argv);
int runTestWorkerOcr(int argc, char** argv);
int runTestWorkerEpub(int argc, char** argv);
int runTestWorkerNativeTypes(int argc, char** argv);
int runTestWorkerNativeRuntime();
int runTestIntegrationIpc(int argc, char** argv);

namespace {

using TestRunner = int (*)(int, char**);

struct TestEntry {
    std::string_view name;
    TestRunner run;
};

constexpr std::array tests {
    TestEntry { "test_worker_document", runTestWorkerDocument },
    TestEntry { "test_worker_page", runTestWorkerPage },
    TestEntry { "test_worker_signature", runTestWorkerSignature },
    TestEntry { "test_plugin_signature", runTestPluginSignature },
    TestEntry { "test_worker_ocr", runTestWorkerOcr },
    TestEntry { "test_worker_epub", runTestWorkerEpub },
    TestEntry { "test_worker_native_types", runTestWorkerNativeTypes },
    TestEntry { "test_integration_ipc", runTestIntegrationIpc },
};

TestRunner findTestRunner(std::string_view name)
{
    for (const auto& test : tests) {
        if (test.name == name)
            return test.run;
    }
    return nullptr;
}

int runAllMuPdfTests(char* programName)
{
    std::array<char*, 2> testArguments { programName, nullptr };
    int failures = 0;

    for (const auto& test : tests) {
        std::fprintf(stderr, "\n=== %s ===\n", test.name.data());
        if (test.run(1, testArguments.data()) != 0)
            failures = 1;
    }

    std::fprintf(stderr, "\n=== test_worker_native_runtime ===\n");
    if (runTestWorkerNativeRuntime() != 0)
        failures = 1;

    return failures;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 1) {
        std::array<char*, 2> applicationArguments { argv[0], nullptr };
        int applicationArgumentCount = 1;
        QGuiApplication application(applicationArgumentCount, applicationArguments.data());
        return runAllMuPdfTests(argv[0]);
    }

    if (argc < 3 || std::string_view(argv[1]) != "--suite") {
        std::fprintf(stderr, "usage: %s --suite <test-name> [QtTest options]\n", argv[0]);
        return 2;
    }

    const std::string_view suite = argv[2];
    if (suite == "test_worker_native_runtime")
        return runTestWorkerNativeRuntime();

    const TestRunner runner = findTestRunner(suite);
    if (!runner) {
        std::fprintf(stderr, "unknown test suite: %s\n", argv[2]);
        return 2;
    }

    std::vector<char*> testArguments;
    testArguments.reserve(static_cast<std::size_t>(argc - 1));
    testArguments.push_back(argv[0]);
    for (int index = 3; index < argc; ++index)
        testArguments.push_back(argv[index]);
    testArguments.push_back(nullptr);
    int testArgumentCount = static_cast<int>(testArguments.size() - 1);

    QGuiApplication application(testArgumentCount, testArguments.data());
    return runner(testArgumentCount, testArguments.data());
}
