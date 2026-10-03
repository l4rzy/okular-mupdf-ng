// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sys/operation_budget.hpp"

#ifdef __linux__
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <unistd.h>

namespace {

void armTimer(timer_t* timer, clockid_t clock, std::chrono::milliseconds duration)
{
    if (duration.count() <= 0) {
        errno = EINVAL;
    } else {
        sigevent event { };
        event.sigev_notify = SIGEV_SIGNAL;
        event.sigev_signo = SIGKILL;
        if (::timer_create(clock, &event, timer) == 0) {
            itimerspec timeout { };
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
            timeout.it_value.tv_sec = seconds.count();
            timeout.it_value.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds).count();
            if (::timer_settime(*timer, 0, &timeout, nullptr) == 0)
                return;
        }
    }
    // Fail closed before executing document code without its hard backstop.
    ::perror("could not arm worker operation budget");
    ::_exit(EXIT_FAILURE);
}

} // namespace
#endif

namespace Mu::Worker::Sys {

OperationBudget::OperationBudget(std::chrono::milliseconds cpu, std::chrono::milliseconds elapsed)
{
#ifdef __linux__
    armTimer(&m_cpu, CLOCK_THREAD_CPUTIME_ID, cpu);
    armTimer(&m_elapsed, CLOCK_MONOTONIC, elapsed);
#else
    (void)cpu;
    (void)elapsed;
#endif
}

OperationBudget::~OperationBudget()
{
#ifdef __linux__
    if (::timer_delete(m_elapsed) != 0 || ::timer_delete(m_cpu) != 0) {
        ::perror("could not remove worker operation budget");
        ::_exit(EXIT_FAILURE);
    }
#endif
}

} // namespace Mu::Worker::Sys
