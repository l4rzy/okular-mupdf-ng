// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_SYS_SANDBOX_HPP
#define MU_WORKER_SYS_SANDBOX_HPP

#include "shared/model/sandbox_status.hpp"

#include <string>
#include <vector>

namespace Mu::Worker::Sandbox {

using Status = ::Mu::Model::SandboxStatus;

/**
 * Restricts the initialized worker process using defense-in-depth security layers.
 *
 * Primary activation phases are descriptor sanitization, namespace isolation, resource limits,
 * Landlock, and Seccomp. MDWE and ambient-capability clearing are optional kernel hardening.
 * Activation is best-effort; the returned protections and diagnostics describe degraded phases.
 * DISABLE_WORKER_SANDBOX diagnostic builds return an inactive status without applying restrictions.
 *
 * @param readOnlyDirectories All directories are optional read-only paths; missing entries are skipped.
 * An empty list enforces Landlock with no read exceptions.
 * @param preservedFds Active file descriptors to exempt from Release descriptor closure and, for standard descriptors,
 * from redirection to /dev/null.
 * @return Detailed status structure indicating which security layers were successfully engaged.
 */
Sandbox::Status activate(const std::vector<std::string>& readOnlyDirectories,
                         const std::vector<int>& preservedFds = { });

} // namespace Mu::Worker::Sandbox

#endif // MU_WORKER_SYS_SANDBOX_HPP
