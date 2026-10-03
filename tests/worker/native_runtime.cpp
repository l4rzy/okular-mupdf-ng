// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ocr/jobs.hpp"
#include "runtime/memory_pressure.hpp"
#include "shared/transport/poll.hpp"
#include "sys/sys.hpp"

#include <QTest>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

using namespace ::Mu::Worker::Engine;
using namespace ::Mu::Worker::Sys;
using ::Mu::IPC::IoResult;
using ::Mu::IPC::MonotonicDeadline;
using ::Mu::IPC::PollLoop;
using ::Mu::IPC::waitForFd;

namespace {

::Mu::Model::OcrResult completeOcrWithoutRecognition(
    int fd, const std::string&, int, const std::string&, float, CancellationCookie*, const std::string&)
{
    FileDescriptor input(fd);
    return { ::Mu::Model::OcrStatus::Success, { } };
}

} // namespace

class TestWorkerNativeRuntime : public QObject {
    Q_OBJECT

private slots:

    void eventFdDispatchesPollCallbacks()
    {
        std::string error;
        auto event = createEventFd(&error);
        QVERIFY(event && error.empty());
        eventfd_t signal = 1;
        QVERIFY(::eventfd_write(event->get(), signal) == 0);
        MonotonicDeadline deadline(std::chrono::milliseconds(100));
        QVERIFY(waitForFd(event->get(), POLLIN, deadline, &error) == IoResult::Complete);
        eventfd_t consumed = 0;
        QVERIFY(::eventfd_read(event->get(), &consumed) == 0 && consumed == 1);

        // Infinite deadline verification
        QVERIFY(MonotonicDeadline::never().pollTimeoutMilliseconds() == -1);
        QVERIFY(MonotonicDeadline().pollTimeoutMilliseconds() == -1);

        PollLoop loop;
        bool dispatched = false;
        QVERIFY(loop.watch(event->get(), POLLIN, [&](short events) {
            QVERIFY(events & POLLIN);
            eventfd_t value = 0;
            QVERIFY(::eventfd_read(event->get(), &value) == 0);
            dispatched = value == 2;
        }));
        signal = 2;
        QVERIFY(::eventfd_write(event->get(), signal) == 0);
        MonotonicDeadline loopDeadline(std::chrono::milliseconds(100));
        QVERIFY(loop.runOnce(loopDeadline, &error) == 1 && dispatched);
        loop.unwatch(event->get());
    }

    void memfdSealsPreventResizing()
    {
        std::string error;
        auto memfd = createMemfd("mupdf-worker-test", 4096, &error);
        QVERIFY(memfd && error.empty());
        const int seals = ::fcntl(memfd->get(), F_GET_SEALS);
        QVERIFY(seals >= 0 && (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) == (F_SEAL_SHRINK | F_SEAL_GROW));
        errno = 0;
        QVERIFY(::ftruncate(memfd->get(), 2048) != 0 && errno == EPERM);
        errno = 0;
        QVERIFY(::ftruncate(memfd->get(), 8192) != 0 && errno == EPERM);
        void* mapped = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memfd->get(), 0);
        QVERIFY(mapped != MAP_FAILED);
        Mapping mapping(mapped, 4096);
        std::memcpy(mapping.data(), "ok", 3);
        QVERIFY(std::strcmp(static_cast<const char*>(mapping.data()), "ok") == 0);
    }

    void cancelledJobsReleaseCapacity()
    {
        // Cancellation invalidates both the job generation and any completion
        // that raced with close/reopen; no stale notification may escape.
        OcrJobs jobs(1, completeOcrWithoutRecognition, 10);
        const int input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(input >= 0);
        const auto job = jobs.submit(input, { }, 0, "eng", 225.0f);
        QVERIFY(job.has_value());
        jobs.cancelAll();
        QVERIFY(jobs.drainNotifications().empty());
        QVERIFY(!jobs.take(*job).has_value());

        // Cancellation must release capacity immediately, without waiting for
        // the detached worker to observe its cancellation cookie.
        const int replacementInput = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(replacementInput >= 0);
        const auto replacementJob = jobs.submit(replacementInput, { }, 0, "eng", 225.0f);
        QVERIFY(replacementJob.has_value());
        jobs.cancelAll();
    }

    void completedResultsHaveOneConsumer()
    {
        std::string error;
        // Completed results are retained until their single consumer takes them.
        OcrJobs completedJobs(1, completeOcrWithoutRecognition, 10);
        const int completedInput = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(completedInput >= 0);
        const auto completedJob = completedJobs.submit(completedInput, { }, 0, "eng", 225.0f);
        QVERIFY(completedJob.has_value());
        auto completionDeadline = MonotonicDeadline::fromMilliseconds(3000);
        QVERIFY(waitForFd(completedJobs.eventFd(), POLLIN, completionDeadline, &error) == IoResult::Complete);
        const auto notifications = completedJobs.drainNotifications();
        QCOMPARE(notifications.size(), std::size_t(1));
        QCOMPARE(notifications.front().id, *completedJob);
        const auto result = completedJobs.take(*completedJob);
        QVERIFY(result);
        QCOMPARE(result->status, ::Mu::Model::OcrStatus::Success);
        QVERIFY(!completedJobs.take(*completedJob).has_value());
    }

    void watchdogCancellationNotifiesAndReleasesCapacity()
    {
        std::string error;
        // A watchdog-cancelled job must notify the host and free capacity after
        // take(), so the next page can run. No PDF or Tesseract is needed.
        const auto runner = +[](int fd,
                                const std::string&,
                                int page,
                                const std::string&,
                                float,
                                CancellationCookie* cookie,
                                const std::string& directory) -> ::Mu::Model::OcrResult {
            FileDescriptor inputFd(fd);
            if (directory != "/test/tessdata")
                return { ::Mu::Model::OcrStatus::Failed, { } };
            if (page == 0) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!cookie->isCancelled() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // Success with partial text deliberately exercises cancellation's
            // override of the runner result.
            return { ::Mu::Model::OcrStatus::Success, { { "text", 0, 0, 1, 1, true } } };
        };
        OcrJobs watchdogJobs(1, runner, 1);
        const ::Mu::Model::OcrStatus expectedStatuses[] = { ::Mu::Model::OcrStatus::Cancelled,
                                                            ::Mu::Model::OcrStatus::Success };
        for (int page = 0; page < 2; ++page) {
            const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            QVERIFY(fd >= 0);
            const auto id = watchdogJobs.submit(fd, { }, page, "eng", 225.0f, "/test/tessdata");
            QVERIFY(id);
            auto completionDeadline = MonotonicDeadline::fromMilliseconds(3000);
            QVERIFY(waitForFd(watchdogJobs.eventFd(), POLLIN, completionDeadline, &error) == IoResult::Complete);
            const auto notifications = watchdogJobs.drainNotifications();
            QVERIFY(notifications.size() == 1 && notifications.front().id == *id && notifications.front().page == page);
            const auto result = watchdogJobs.take(*id);
            QVERIFY(result && result->status == expectedStatuses[page]);
            QVERIFY(result->boxes.size() == (page == 0 ? 0U : 1U));
            QVERIFY(!watchdogJobs.take(*id));
            QVERIFY(watchdogJobs.drainNotifications().empty());
        }
    }

    void takeAndCancelDoNotDeadlock()
    {
        // take() and cancelAll() must use the same lock order. Run them together
        // repeatedly so a future change cannot reintroduce the lock inversion.
        for (int iteration = 0; iteration < 256; ++iteration) {
            OcrJobs concurrentJobs(1, completeOcrWithoutRecognition, 10);
            const int inputFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            QVERIFY(inputFd >= 0);
            const auto concurrentJob = concurrentJobs.submit(inputFd, { }, 0, "eng", 225.0f);
            QVERIFY(concurrentJob.has_value());

            std::atomic<int> ready { 0 };
            std::atomic_bool start { false };
            std::thread takeThread([&] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                (void)concurrentJobs.take(*concurrentJob);
            });
            std::thread cancelThread([&] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                concurrentJobs.cancelAll();
            });
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            start.store(true, std::memory_order_release);
            takeThread.join();
            cancelThread.join();
            QVERIFY(!concurrentJobs.take(*concurrentJob));
            QVERIFY(concurrentJobs.drainNotifications().empty());
        }
    }

    void destructionClosesCompletionDescriptor()
    {
        // Destruction closes the completion channel even with a submitted job.
        int completionFd = -1;
        {
            OcrJobs jobs(1, completeOcrWithoutRecognition, 10);
            completionFd = jobs.eventFd();
            QVERIFY(completionFd >= 0);
            const int input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            QVERIFY(input >= 0);
            QVERIFY(jobs.submit(input, { }, 0, "eng", 225.0f));
        }
        errno = 0;
        QVERIFY(::fcntl(completionFd, F_GETFD) == -1 && errno == EBADF);
    }

    void idleTrimPolicyMatchesThresholds()
    {
        // Idle-trim decision is pure and table-driven: trim only after an idle
        // pause with enough accumulated render pressure, never on the hot path.
        // Thresholds scale with the configured aggressiveness preset.
        struct PressureCase {
            std::int32_t idleTrim;
            std::uint64_t renders;
            std::uint64_t bytes;
            long long idleMs;
            long long sinceTrimMs;
            bool expected;
        };

        constexpr std::uint64_t MiB = 1024ULL * 1024ULL;
        namespace MemoryPressure = ::Mu::Worker::Runtime::MemoryPressure;
        using IdleTrim = ::Mu::Model::IdleTrimLevel;
        const auto balanced = MemoryPressure::idleTrimThresholdsForLevel(IdleTrim::Balanced);
        const long long idleGate = balanced.idleMinMs;
        const long long trimGate = balanced.sinceTrimMinMs;
        const PressureCase pressureCases[] = {
            // Balanced preserves the historical behavior.
            { IdleTrim::Balanced, 0, 0, 5000, 5000, false }, // idle but no pressure
            { IdleTrim::Balanced, 20, 0, idleGate, trimGate, true }, // render-count threshold
            { IdleTrim::Balanced, 19, 0, 5000, 5000, false }, // just below render threshold
            { IdleTrim::Balanced, 1, 128 * MiB, idleGate, trimGate, true }, // byte threshold
            { IdleTrim::Balanced, 1, 128 * MiB - 1, idleGate, trimGate, false }, // just below byte threshold
            { IdleTrim::Balanced, 4, 32 * MiB, idleGate, trimGate, true }, // large frames arm early
            { IdleTrim::Balanced, 3, 32 * MiB, 5000, 5000, false }, // large bytes but too few renders
            { IdleTrim::Balanced, 4, 32 * MiB - 1, 5000, 5000, false }, // enough renders but below large bytes
            { IdleTrim::Balanced, 20, 0, idleGate - 1, 5000, false }, // not idle long enough
            { IdleTrim::Balanced, 20, 0, 5000, trimGate - 1, false }, // trimmed too recently
            // Conservative needs roughly twice the pressure and idle time.
            { IdleTrim::Conservative, 20, 0, 5000, 5000, false },
            { IdleTrim::Conservative, 40, 0, 5000, 5000, true },
            { IdleTrim::Conservative, 20, 0, idleGate, 5000, false },
            // Aggressive arms at roughly half the pressure and idle time.
            { IdleTrim::Aggressive, 10, 0, 1000, 1000, true },
            { IdleTrim::Aggressive, 9, 0, 5000, 5000, false },
            { IdleTrim::Aggressive, 1, 64 * MiB, 1000, 1000, true },
            { IdleTrim::Aggressive, 20, 0, idleGate - 1, 5000, true },
            // Unknown levels degrade to Balanced.
            { 99, 20, 0, idleGate, trimGate, true },
            { 99, 19, 0, 5000, 5000, false },
        };
        for (const auto& testCase : pressureCases) {
            const auto idleTrim = MemoryPressure::normalizeIdleTrim(testCase.idleTrim);
            if (testCase.idleTrim == 99)
                QVERIFY(idleTrim == IdleTrim::Balanced);
            QVERIFY(MemoryPressure::shouldIdleTrim(testCase.renders,
                                                   testCase.bytes,
                                                   testCase.idleMs,
                                                   testCase.sinceTrimMs,
                                                   MemoryPressure::idleTrimThresholdsForLevel(idleTrim))
                    == testCase.expected);
        }

        // Off disables trimming; the poll quantum stays a plain keepalive.
        QVERIFY(MemoryPressure::normalizeIdleTrim(IdleTrim::Off) == IdleTrim::Off);
        QVERIFY(MemoryPressure::idleTrimPollMsForLevel(IdleTrim::Off) == 2000);
        QVERIFY(MemoryPressure::idleTrimPollMsForLevel(IdleTrim::Balanced) == idleGate);
        QVERIFY(MemoryPressure::idleTrimPollMsForLevel(99) == idleGate);
        // Trim depth: lower store-percentage target evicts more.
        QVERIFY(MemoryPressure::idleTrimThresholdsForLevel(IdleTrim::Conservative).storePercent == 75);
        QVERIFY(MemoryPressure::idleTrimThresholdsForLevel(IdleTrim::Balanced).storePercent == 50);
        QVERIFY(MemoryPressure::idleTrimThresholdsForLevel(IdleTrim::Aggressive).storePercent == 25);
    }
};

int runTestWorkerNativeRuntime(int argc, char** argv)
{
    TestWorkerNativeRuntime test;
    return QTest::qExec(&test, argc, argv);
}

#include "native_runtime.moc"
