// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_SANDBOX_HPP
#define MUPDF_WORKER_SANDBOX_HPP

#include "shared/model/types.hpp"

#include <string>
#include <vector>

namespace Mu::Worker::Sandbox {

using Status = ::Mu::Model::SandboxStatus;

/// Applies Linux resource limits to the worker process.
///
/// All Linux builds cap virtual memory at 4 GiB and CPU time at 60/120 seconds.
/// Release builds additionally disable core dumps and mark the process non-dumpable
/// to avoid exposing document contents or keys after a crash.
bool applyResourceLimits(Status& status);

/**
 * Restricts the initialized worker process using defense-in-depth security layers.
 *
 * Primary activation phases are descriptor sanitization, namespace isolation, resource limits,
 * Landlock, and Seccomp. MDWE and ambient-capability clearing are optional kernel hardening.
 * Activation is best-effort; the returned status and reason describe degraded phases.
 *
 * @param readOnlyDirectories The first directory is required; subsequent directories are optional read-only paths.
 * @param preservedFds Active file descriptors to exempt from inherited descriptor closure.
 * @return Detailed status structure indicating which security layers were successfully engaged.
 */
Sandbox::Status activate(const std::vector<std::string>& readOnlyDirectories,
                         const std::vector<int>& preservedFds = { });

} // namespace Mu::Worker::Sandbox

#endif
