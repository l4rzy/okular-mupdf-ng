// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_SYS_OPERATION_BUDGET_HPP
#define MU_WORKER_SYS_OPERATION_BUDGET_HPP

#include <chrono>
#ifdef __linux__
#include <time.h>
#endif

#include "engine/constants.hpp"

namespace Mu::Worker::Sys {

/// Bounds one operation's executing-thread CPU and elapsed time. Expiry kills
/// the worker: interrupted document engines cannot safely be reused. Construct
/// outside MuPDF fz_try regions so longjmp cannot bypass timer cleanup.
class OperationBudget {
public:
    explicit OperationBudget(
        std::chrono::milliseconds cpu = std::chrono::seconds(Engine::Constant::OperationCpuSeconds),
        std::chrono::milliseconds elapsed = std::chrono::seconds(Engine::Constant::OperationElapsedSeconds));
    ~OperationBudget();
    OperationBudget(const OperationBudget&) = delete;
    OperationBudget& operator=(const OperationBudget&) = delete;

private:
#ifdef __linux__
    timer_t m_cpu { };
    timer_t m_elapsed { };
#endif
};

} // namespace Mu::Worker::Sys

#endif // MU_WORKER_SYS_OPERATION_BUDGET_HPP
