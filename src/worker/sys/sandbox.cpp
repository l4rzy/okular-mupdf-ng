// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sys/sandbox.hpp"

#ifdef __linux__
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef MUPDF_HAVE_LIBSECCOMP
#include <linux/sched.h>
#include <seccomp.h>
#endif
#endif // __linux__

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>

namespace Mu::Worker::Sandbox {

#ifdef __linux__
namespace {

void recordIssue(Status& status, std::string_view issue)
{
    if (status.reason.empty())
        status.reason = issue;
    else
        status.reason += "; " + std::string(issue);
}

bool recordErrno(Status& status, std::string_view operation)
{
    const int error = errno;
    recordIssue(status, std::string(operation) + ": " + std::strerror(error));
    return false;
}

bool writeProcFile(Status& status, const char* path, std::string_view name, std::string_view contents)
{
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return recordErrno(status, std::string("Opening ") + std::string(name));

    std::size_t written = 0;
    while (written < contents.size()) {
        const ssize_t count = ::write(fd, contents.data() + written, contents.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            recordErrno(status, std::string("Writing ") + std::string(name));
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    ::close(fd);
    return true;
}

int landlockCreate(const landlock_ruleset_attr* attributes, size_t size, __u32 flags)
{
    return static_cast<int>(::syscall(SYS_landlock_create_ruleset, attributes, size, flags));
}

// Landlock capability is probed with a zero-size ruleset request that returns
// the supported ABI version. Access bits are enabled only when both the build
// headers expose them and the running kernel reports the corresponding ABI.
bool activateLandlock(const std::vector<std::string>& readOnlyDirectories, Status& status)
{
    // Probe supported Landlock ABI version
    const int abi = landlockCreate(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0)
        return recordErrno(status, "Probing Landlock");
    if (abi < 1) {
        recordIssue(status, "Landlock unavailable");
        return false;
    }
    status.landlockAbi = abi;

    // Define access classes to restrict. Unhandled access classes are left unrestricted,
    // so we explicitly include all file creation, write, modification, device, and link rights.
    landlock_ruleset_attr ruleset { };
    ruleset.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE
        | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO
        | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        ruleset.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        ruleset.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5)
        ruleset.handled_access_fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif

    // Create the Landlock ruleset file descriptor
    const int rulesetFd = landlockCreate(&ruleset, sizeof(ruleset), 0);
    if (rulesetFd < 0) {
        return recordErrno(status, "Landlock ruleset creation failed");
    }

    // Helper lambda to grant read-only access to a specific directory tree
    const auto addPathRule = [&](const std::string& directory, __u64 allowedAccess, bool required) {
        if (directory.empty())
            return !required;

        // Open directory with O_PATH to get a lightweight descriptor for Landlock rule creation
        const int parentFd = ::open(directory.c_str(), O_PATH | O_CLOEXEC);
        if (parentFd < 0) {
            if (required)
                return recordErrno(status, "Landlock default tessdata directory unavailable");
            return false;
        }
        landlock_path_beneath_attr rule { };
        rule.allowed_access = allowedAccess;
        rule.parent_fd = parentFd;
        if (static_cast<int>(::syscall(SYS_landlock_add_rule, rulesetFd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0)) < 0) {
            if (required)
                recordErrno(status, "Landlock rule creation failed");
            ::close(parentFd);
            return false;
        }
        ::close(parentFd);
        return true;
    };

    // The first directory is the build-time default and is required. Additional
    // configured directories are optional and must not disable Landlock.
    if (readOnlyDirectories.empty()
        || !addPathRule(
            readOnlyDirectories.front(), LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR, true)) {
        if (readOnlyDirectories.empty())
            recordIssue(status, "Landlock default tessdata directory unavailable");
        ::close(rulesetFd);
        status.landlock = false;
        return false;
    }
    for (std::size_t index = 1; index < readOnlyDirectories.size(); ++index) {
        addPathRule(readOnlyDirectories[index], LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR, false);
    }

    // Enforce the ruleset only after every permitted tessdata directory has been added.
    bool success = true;
    if (static_cast<int>(::syscall(SYS_landlock_restrict_self, rulesetFd, 0)) < 0) {
        recordErrno(status, "Landlock activation failed");
        success = false;
    }
    ::close(rulesetFd);
    status.landlock = success;
    return success;
}

/// Activates Linux namespace isolation (User, Network, and IPC namespaces).
///
/// Creating an unprivileged User namespace allows unsharing Network and IPC namespaces
/// without requiring CAP_SYS_ADMIN/root privileges on the host system.
bool activateNamespaces(Status& status)
{
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();

    // Unshare User, Network, and IPC namespaces.
    // - CLONE_NEWUSER: Grants root-equivalent capabilities inside the container namespace.
    // - CLONE_NEWNET: Detaches network interfaces, preventing outbound network access.
    // - CLONE_NEWIPC: Isolates System V IPC and POSIX message queues.
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWIPC) != 0) {
        return recordErrno(status, "Namespace isolation unshare failed");
    }

    // Map the process's real host UID to UID 0 (root) inside the new user namespace.
    const std::string uidMapping = "0 " + std::to_string(uid) + " 1\n";
    if (!writeProcFile(status, "/proc/self/uid_map", "uid_map", uidMapping)) {
        return false;
    }

    // Disable setgroups() system call inside the user namespace.
    // Linux requires setgroups to be explicitly disabled before unprivileged gid_map can be written.
    if (!writeProcFile(status, "/proc/self/setgroups", "setgroups", "deny\n")) {
        return false;
    }

    // Map the process's real host GID to GID 0 (root) inside the new user namespace.
    const std::string gidMapping = "0 " + std::to_string(gid) + " 1\n";
    if (!writeProcFile(status, "/proc/self/gid_map", "gid_map", gidMapping)) {
        return false;
    }

    status.linuxNamespace = true;
    return true;
}

/// Enforces Memory-Deny-Write-Execute (MDWE / W^X) protection on Linux >= 6.3.
/// Permanently prevents turning writable memory pages into executable memory.
bool tryEnableMemoryProtection(Status& status)
{
    if (::prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN, 0, 0, 0) == 0) {
        status.memoryProtection = true;
        return true;
    }
    return false;
}

bool requireNoNewPrivileges(Status& status)
{
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0)
        return true;
    return recordErrno(status, "Setting no-new-privileges");
}

void tryClearAmbientCapabilities()
{
#ifdef PR_CAP_AMBIENT
    // Optional: kernels without ambient-capability support retain no privilege
    // we intentionally add, so failure does not block worker startup.
    (void)::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
#endif
}

} // namespace

/// Applies the configured Linux memory, CPU, and release-only core-dump limits.
bool applyResourceLimits(Status& status)
{
#ifdef __linux__
    bool ok = true;
#ifndef MU_DEBUG_ENABLED
    // Disable core dumps (RLIMIT_CORE = 0 & PR_SET_DUMPABLE = 0) on release builds
    // Prevents sensitive document contents or crypto keys in worker RAM from being written to disk on crash.
    struct rlimit rlCore { 0, 0 };
    if (::setrlimit(RLIMIT_CORE, &rlCore) != 0) {
        recordErrno(status, "RLIMIT_CORE");
        ok = false;
    }
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        recordErrno(status, "PR_SET_DUMPABLE");
        ok = false;
    }
#endif // MU_DEBUG_ENABLED

    // Cap virtual memory address space (RLIMIT_AS = 4 GB)
    // Guards against malicious or malformed documents triggering huge allocation rendering bombs.
    constexpr rlim_t FourGigabytes = 4096ULL * 1024U * 1024U;
    struct rlimit rlAs { FourGigabytes, FourGigabytes };
    if (::setrlimit(RLIMIT_AS, &rlAs) != 0) {
        recordErrno(status, "RLIMIT_AS");
        ok = false;
    }

    // Cap CPU execution time (RLIMIT_CPU = 60s soft / 120s hard)
    // Protects against infinite loop rendering bombs.
    struct rlimit rlCpu { 60, 120 };
    if (::setrlimit(RLIMIT_CPU, &rlCpu) != 0) {
        recordErrno(status, "RLIMIT_CPU");
        ok = false;
    }
    status.resourceLimits = ok;
    return ok;
#else
    status.resourceLimits = false;
    return false;
#endif
}

namespace {

#ifndef MU_DEBUG_ENABLED
bool closeInheritedDescriptors(const std::vector<int>& preservedFds, Status& status)
{
#ifdef __linux__
    std::vector<int> preserved;
    for (const int fd : preservedFds) {
        if (fd >= 3)
            preserved.push_back(fd);
    }
    std::sort(preserved.begin(), preserved.end());
    preserved.erase(std::unique(preserved.begin(), preserved.end()), preserved.end());

    unsigned int first = 3;
    for (const int preservedFd : preserved) {
        const unsigned int preservedValue = static_cast<unsigned int>(preservedFd);
        if (first < preservedValue && ::syscall(SYS_close_range, first, preservedValue - 1, 0) != 0) {
            return recordErrno(status, "Closing inherited descriptors");
        }
        if (preservedValue == std::numeric_limits<unsigned int>::max())
            return true;
        first = preservedValue + 1;
    }

    if (::syscall(SYS_close_range, first, std::numeric_limits<unsigned int>::max(), 0) == 0)
        return true;
    return recordErrno(status, "Closing inherited descriptors");
#else
    (void)preservedFds;
    (void)status;
    return true;
#endif
}

bool redirectStandardDescriptors(Status& status)
{
#ifdef __linux__
    const int nullFd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullFd < 0) {
        return recordErrno(status, "Opening /dev/null");
    }

    bool success = true;
    for (const int standardFd : { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO }) {
        if (::dup2(nullFd, standardFd) < 0) {
            recordErrno(status, "Redirecting standard descriptor");
            success = false;
        }
    }
    if (nullFd > STDERR_FILENO)
        ::close(nullFd);
    return success;
#else
    (void)status;
    return true;
#endif
}
#endif

} // namespace

#ifdef MUPDF_HAVE_LIBSECCOMP
/// Configures and installs the Linux Seccomp (syscall filtering) BPF filter.
///
/// Default action is SCMP_ACT_KILL_PROCESS. Only explicitly allowlisted syscalls
/// necessary for rendering, IPC, and threading are permitted.
bool activateSeccomp(Status& status)
{
    // Initialize seccomp filter with default-kill action
    scmp_filter_ctx filter = seccomp_init(SCMP_ACT_KILL_PROCESS);
    if (!filter) {
        status.reason = "Seccomp filter allocation failed";
        return false;
    }

    int error = 0;
    const auto allowList = [&](std::initializer_list<int> syscalls) {
        for (int nr : syscalls) {
            if (!error && nr >= 0)
                error = seccomp_rule_add(filter, SCMP_ACT_ALLOW, nr, 0);
        }
    };

    allowList({
    // accept4() must remain: the worker accepts the authenticated plugin on
    // the control listener it inherited before activation. Creating new
    // sockets or connecting to new AF_UNIX endpoints (socket, bind, connect,
    // listen, accept, socketpair) is intentionally absent from this allowlist,
    // so a compromised worker cannot bridge the session bus, X11, ssh-agent,
    // or any other same-user service.
#ifdef __NR_accept4
        __NR_accept4,
#endif
#ifdef __NR_access
        __NR_access,
#endif
#ifdef __NR_arch_prctl
        __NR_arch_prctl,
#endif
#ifdef __NR_brk
        __NR_brk,
#endif
#ifdef __NR_clock_gettime
        __NR_clock_gettime,
#endif
#ifdef __NR_clock_nanosleep
        __NR_clock_nanosleep,
#endif
#ifdef __NR_close
        __NR_close,
#endif
#ifdef __NR_dup
        __NR_dup,
#endif
#ifdef __NR_dup2
        __NR_dup2,
#endif
#ifdef __NR_dup3
        __NR_dup3,
#endif
#ifdef __NR_epoll_create1
        __NR_epoll_create1,
#endif
#ifdef __NR_epoll_ctl
        __NR_epoll_ctl,
#endif
#ifdef __NR_epoll_wait
        __NR_epoll_wait,
#endif
#ifdef __NR_eventfd2
        __NR_eventfd2,
#endif
#ifdef __NR_exit
        __NR_exit,
#endif
#ifdef __NR_exit_group
        __NR_exit_group,
#endif
#ifdef __NR_faccessat
        __NR_faccessat,
#endif
#ifdef __NR_faccessat2
        __NR_faccessat2,
#endif
#ifdef __NR_fadvise64
        __NR_fadvise64,
#endif
#ifdef __NR_fcntl
        __NR_fcntl,
#endif
#ifdef __NR_fdatasync
        __NR_fdatasync,
#endif
#ifdef __NR_fstat
        __NR_fstat,
#endif
#ifdef __NR_fstatfs
        __NR_fstatfs,
#endif
#ifdef __NR_fsync
        __NR_fsync,
#endif
#ifdef __NR_ftruncate
        __NR_ftruncate,
#endif
#ifdef __NR_futex
        __NR_futex,
#endif
#ifdef __NR_getcwd
        __NR_getcwd,
#endif
#ifdef __NR_getcpu
        __NR_getcpu,
#endif
#ifdef __NR_getdents64
        __NR_getdents64,
#endif
#ifdef __NR_getegid
        __NR_getegid,
#endif
#ifdef __NR_geteuid
        __NR_geteuid,
#endif
#ifdef __NR_getgid
        __NR_getgid,
#endif
#ifdef __NR_getpid
        __NR_getpid,
#endif
#ifdef __NR_getppid
        __NR_getppid,
#endif
#ifdef __NR_getrandom
        __NR_getrandom,
#endif
#ifdef __NR_getrlimit
        __NR_getrlimit,
#endif
#ifdef __NR_getsockopt
        __NR_getsockopt,
#endif
#ifdef __NR_gettid
        __NR_gettid,
#endif
#ifdef __NR_getuid
        __NR_getuid,
#endif
#ifdef __NR_ioctl
        __NR_ioctl,
#endif
#ifdef __NR_lseek
        __NR_lseek,
#endif
#ifdef __NR_madvise
        __NR_madvise,
#endif
#ifdef __NR_membarrier
        __NR_membarrier,
#endif
#ifdef __NR_memfd_create
        __NR_memfd_create,
#endif
#ifdef __NR_mmap
        __NR_mmap,
#endif
#ifdef __NR_mprotect
        __NR_mprotect,
#endif
#ifdef __NR_mremap
        __NR_mremap,
#endif
#ifdef __NR_munmap
        __NR_munmap,
#endif
#ifdef __NR_nanosleep
        __NR_nanosleep,
#endif
#ifdef __NR_newfstatat
        __NR_newfstatat,
#endif
#ifdef __NR_openat
        __NR_openat,
#endif
#ifdef __NR_poll
        __NR_poll,
#endif
#ifdef __NR_ppoll
        __NR_ppoll,
#endif
#ifdef __NR_prctl
        __NR_prctl,
#endif
#ifdef __NR_pread64
        __NR_pread64,
#endif
#ifdef __NR_prlimit64
        __NR_prlimit64,
#endif
#ifdef __NR_pselect6
        __NR_pselect6,
#endif
#ifdef __NR_pwrite64
        __NR_pwrite64,
#endif
#ifdef __NR_read
        __NR_read,
#endif
#ifdef __NR_readlinkat
        __NR_readlinkat,
#endif
#ifdef __NR_recvfrom
        __NR_recvfrom,
#endif
#ifdef __NR_recvmsg
        __NR_recvmsg,
#endif
#ifdef __NR_rseq
        __NR_rseq,
#endif
#ifdef __NR_rt_sigaction
        __NR_rt_sigaction,
#endif
#ifdef __NR_rt_sigprocmask
        __NR_rt_sigprocmask,
#endif
#ifdef __NR_rt_sigreturn
        __NR_rt_sigreturn,
#endif
#ifdef __NR_sched_getaffinity
        __NR_sched_getaffinity,
#endif
#ifdef __NR_sched_setaffinity
        __NR_sched_setaffinity,
#endif
#ifdef __NR_sched_yield
        __NR_sched_yield,
#endif
#ifdef __NR_sendmsg
        __NR_sendmsg,
#endif
#ifdef __NR_sendto
        __NR_sendto,
#endif
#ifdef __NR_set_robust_list
        __NR_set_robust_list,
#endif
#ifdef __NR_set_tid_address
        __NR_set_tid_address,
#endif
#ifdef __NR_setsockopt
        __NR_setsockopt,
#endif
#ifdef __NR_shutdown
        __NR_shutdown,
#endif
#ifdef __NR_statx
        __NR_statx,
#endif
#ifdef __NR_sysinfo
        __NR_sysinfo,
#endif
#ifdef __NR_tgkill
        __NR_tgkill,
#endif
#ifdef __NR_timerfd_create
        __NR_timerfd_create,
#endif
#ifdef __NR_timerfd_settime
        __NR_timerfd_settime,
#endif
#ifdef __NR_uname
        __NR_uname,
#endif
#ifdef __NR_write
        __NR_write,
#endif
#ifdef __NR_writev
        __NR_writev,
#endif
    });

// OCR and MuPDF create threads, but the worker must not create child
// processes. glibc may try clone3 first; make it fall back to clone,
// where the thread flag can be checked directly.
#ifdef __NR_clone
    if (!error) {
        const struct scmp_arg_cmp cloneThread = { 0, SCMP_CMP_MASKED_EQ, CLONE_THREAD, CLONE_THREAD };
        error = seccomp_rule_add_array(filter, SCMP_ACT_ALLOW, __NR_clone, 1, &cloneThread);
    }
#else
    error = -ENOSYS;
    status.reason = "Required syscall is unavailable: clone";
#endif

#ifdef __NR_clone3
    if (!error)
        error = seccomp_rule_add(filter, SCMP_ACT_ERRNO(ENOSYS), __NR_clone3, 0);
#endif

    if (!error && seccomp_attr_set(filter, SCMP_FLTATR_CTL_LOG, 1) != 0)
        error = -EINVAL;
    if (error || seccomp_load(filter) != 0) {
        seccomp_release(filter);
        recordIssue(status, "Seccomp activation failed");
        return false;
    }
    seccomp_release(filter);
    status.seccomp = true;
    return true;
}
#else
bool activateSeccomp(Status& status)
{
    recordIssue(status, "libseccomp was not available at build time");
    return false;
}
#endif

#endif // __linux__

/// Main entry point for sandboxing the okular-mupdf-worker executable.
///
/// Runs the sandbox phases in activation order. Primary controls are required for a fully
/// hardened status; startup remains best-effort when a phase is unavailable.
Sandbox::Status activate(const std::vector<std::string>& readOnlyDirectories, const std::vector<int>& preservedFds)
{
    Sandbox::Status status;
#ifndef __linux__
    status.reason = "sandboxing is supported only on Linux";
    return status;
#else
    // Required: prevent privilege gain through setuid/setgid binaries or execve.
    if (!requireNoNewPrivileges(status))
        return status;

    // Optional: clear ambient capabilities when the kernel exposes the interface.
    tryClearAmbientCapabilities();

    // Optional: enforce W^X when supported by the running kernel.
    tryEnableMemoryProtection(status);
#ifndef MU_DEBUG_ENABLED
    // Release: keep standard descriptors available to libraries.
    redirectStandardDescriptors(status);
    // Release: remove all inherited descriptors except the active IPC channels.
    closeInheritedDescriptors(preservedFds, status);
#else
    // Debug: retain parent descriptors so worker logs remain visible.
    (void)preservedFds;
#endif
    // Required: isolate user, network, and IPC namespaces.
    activateNamespaces(status);
    // Required: prevent resource exhaustion and disable core-dump exposure.
    applyResourceLimits(status);
    // Required: restrict filesystem access to the configured read-only trees.
    activateLandlock(readOnlyDirectories, status);
    // Required: enforce the syscall allowlist with a kill-by-default filter.
    activateSeccomp(status);

    return status;
#endif
}

} // namespace Mu::Worker::Sandbox
