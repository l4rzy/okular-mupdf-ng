// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ocr/jobs.hpp"
#include "runtime/memory_pressure.hpp"
#include "shared/transport/poll.hpp"
#include "sys/sys.hpp"

#include <atomic>
#include <cassert>
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

int runTestWorkerNativeRuntime()
{
    std::string error;
    auto event = createEventFd(&error);
    assert(event && error.empty());
    eventfd_t signal = 1;
    assert(::eventfd_write(event->get(), signal) == 0);
    MonotonicDeadline deadline(std::chrono::milliseconds(100));
    assert(waitForFd(event->get(), POLLIN, deadline, &error) == IoResult::Complete);
    eventfd_t consumed = 0;
    assert(::eventfd_read(event->get(), &consumed) == 0 && consumed == 1);

    // Infinite deadline verification
    assert(MonotonicDeadline::never().pollTimeoutMilliseconds() == -1);
    assert(MonotonicDeadline().pollTimeoutMilliseconds() == -1);

    PollLoop loop;
    bool dispatched = false;
    assert(loop.watch(event->get(), POLLIN, [&](short events) {
        assert(events & POLLIN);
        eventfd_t value = 0;
        assert(::eventfd_read(event->get(), &value) == 0);
        dispatched = value == 2;
    }));
    signal = 2;
    assert(::eventfd_write(event->get(), signal) == 0);
    MonotonicDeadline loopDeadline(std::chrono::milliseconds(100));
    assert(loop.runOnce(loopDeadline, &error) == 1 && dispatched);
    loop.unwatch(event->get());

    auto memfd = createMemfd("mupdf-worker-test", 4096, &error);
    assert(memfd && error.empty());
    void* mapped = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memfd->get(), 0);
    assert(mapped != MAP_FAILED);
    Mapping mapping(mapped, 4096);
    std::memcpy(mapping.data(), "ok", 3);
    assert(std::strcmp(static_cast<const char*>(mapping.data()), "ok") == 0);

    // Cancellation invalidates both the job generation and any completion
    // that raced with close/reopen; no stale notification may escape.
    OcrJobs jobs(1);
    const int input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(input >= 0);
    const auto job = jobs.submit(input, { }, 0, "eng", 225.0f);
    assert(job.has_value());
    jobs.cancelAll();
    assert(jobs.drainNotifications().empty());
    assert(!jobs.take(*job).has_value());

    // Cancellation must release capacity immediately, without waiting for
    // the detached worker to observe its cancellation cookie.
    const int replacementInput = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(replacementInput >= 0);
    const auto replacementJob = jobs.submit(replacementInput, { }, 0, "eng", 225.0f);
    assert(replacementJob.has_value());
    jobs.cancelAll();

    // Completed results are retained until their single consumer takes them.
    OcrJobs completedJobs(1);
    const int completedInput = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(completedInput >= 0);
    const auto completedJob = completedJobs.submit(completedInput, { }, 0, "eng", 225.0f);
    assert(completedJob.has_value());
    std::optional<OcrJobs::Notification> notification;
    for (int attempt = 0; attempt < 100 && !notification; ++attempt) {
        const auto notifications = completedJobs.drainNotifications();
        if (!notifications.empty())
            notification = notifications.front();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(notification && notification->id == *completedJob);
    assert(completedJobs.take(*completedJob).has_value());
    assert(!completedJobs.take(*completedJob).has_value());

    // take() and cancelAll() must use the same lock order. Run them together
    // repeatedly so a future change cannot reintroduce the lock inversion.
    for (int iteration = 0; iteration < 256; ++iteration) {
        OcrJobs concurrentJobs(1);
        const int inputFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(inputFd >= 0);
        const auto concurrentJob = concurrentJobs.submit(inputFd, { }, 0, "eng", 225.0f);
        assert(concurrentJob.has_value());

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
    }

    // Destruction of OcrJobs while background threads are active must safely
    // reset the eventfd so no subsequent write occurs on a reused descriptor.
    for (int iteration = 0; iteration < 64; ++iteration) {
        {
            OcrJobs dyingJobs(2);
            const int input1 = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            assert(input1 >= 0);
            (void)dyingJobs.submit(input1, { }, 0, "eng", 225.0f);
        }
        // Allocate and close descriptors to verify no stray writes corrupt newly opened FDs
        int dummyFds[4] { -1, -1, -1, -1 };
        for (auto& fd : dummyFds) {
            fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            assert(fd >= 0);
        }
        for (auto& fd : dummyFds) {
            ::close(fd);
        }
    }

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
            assert(idleTrim == IdleTrim::Balanced);
        assert(MemoryPressure::shouldIdleTrim(testCase.renders,
                                              testCase.bytes,
                                              testCase.idleMs,
                                              testCase.sinceTrimMs,
                                              MemoryPressure::idleTrimThresholdsForLevel(idleTrim))
               == testCase.expected);
    }

    // Off disables trimming; the poll quantum stays a plain keepalive.
    assert(MemoryPressure::normalizeIdleTrim(IdleTrim::Off) == IdleTrim::Off);
    assert(MemoryPressure::idleTrimPollMsForLevel(IdleTrim::Off) == 2000);
    assert(MemoryPressure::idleTrimPollMsForLevel(IdleTrim::Balanced) == idleGate);
    assert(MemoryPressure::idleTrimPollMsForLevel(99) == idleGate);
    // Trim depth: lower store-percentage target evicts more.
    assert(MemoryPressure::idleTrimThresholdsForLevel(IdleTrim::Conservative).storePercent == 75);
    assert(MemoryPressure::idleTrimThresholdsForLevel(IdleTrim::Balanced).storePercent == 50);
    assert(MemoryPressure::idleTrimThresholdsForLevel(IdleTrim::Aggressive).storePercent == 25);
    return 0;
}
