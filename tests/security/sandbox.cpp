#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "sys/sandbox.hpp"

namespace {

struct ProbeContext {
    std::string allowedDirectory;
    std::string allowedFile;
    std::string outsideFile;
    std::string requiredDirectory;
    std::string optionalDirectory;
    int inheritedFd = -1;
    int unpreservedFd = -1;
};

struct ProbeResult {
    std::uint8_t landlockActive = 0;
    std::uint8_t seccompActive = 0;
    std::uint8_t linuxNamespaceActive = 0;
    std::uint8_t resourceLimitsActive = 0;
    std::uint8_t memoryProtectionActive = 0;
    std::uint8_t allowedRead = 0;
    std::uint8_t outsideReadDenied = 0;
    std::uint8_t pathWriteDenied = 0;
    std::uint8_t createDenied = 0;
    std::uint8_t inheritedRead = 0;
    std::uint8_t inheritedWrite = 0;
    std::uint8_t unpreservedClosed = 0;
    std::uint8_t launcherPipeClosed = 0;
    std::uint8_t threadAllowed = 0;
};

enum class ForbiddenOperation {
    Fork,
    Exec,
    InternetSocket,
    UnixSocket,
};

bool writeProbeResult(int fd, const ProbeResult& result)
{
    const auto* data = reinterpret_cast<const char*>(&result);
    std::size_t written = 0;
    while (written < sizeof(result)) {
        const ssize_t count = ::write(fd, data + written, sizeof(result) - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        written += static_cast<std::size_t>(count);
    }
    return true;
}

bool readProbeResult(int fd, ProbeResult* result)
{
    auto* data = reinterpret_cast<char*>(result);
    std::size_t received = 0;
    while (received < sizeof(*result)) {
        const ssize_t count = ::read(fd, data + received, sizeof(*result) - received);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}

bool runSandboxProbe(const ProbeContext& context, ProbeResult* result)
{
    int pipeFds[2];
    if (::pipe(pipeFds) != 0)
        return false;
    int launcherPipe[2];
    if (::pipe(launcherPipe) != 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        ::close(launcherPipe[0]);
        ::close(launcherPipe[1]);
        return false;
    }
    if (pid == 0) {
        ::close(pipeFds[0]);
        ::close(launcherPipe[0]);
        if (::dup2(launcherPipe[1], STDOUT_FILENO) < 0)
            _exit(1);
        ::close(launcherPipe[1]);
        ProbeResult probe;
        const auto status = ::Mu::Worker::Sandbox::activate({ context.requiredDirectory, context.optionalDirectory },
                                                            { pipeFds[1], context.inheritedFd });
        probe.landlockActive = status.landlock;
        probe.seccompActive = status.seccomp;
        probe.linuxNamespaceActive = status.linuxNamespace;
        probe.resourceLimitsActive = status.resourceLimits;
        probe.memoryProtectionActive = status.memoryProtection;
        probe.unpreservedClosed =
            context.unpreservedFd >= 0 && ::fcntl(context.unpreservedFd, F_GETFD) < 0 && errno == EBADF;

        if (status.landlock) {
            const int allowed = ::open(context.allowedFile.c_str(), O_RDONLY | O_CLOEXEC);
            if (allowed >= 0) {
                char byte = 0;
                probe.allowedRead = ::read(allowed, &byte, 1) == 1;
                ::close(allowed);
            }

            const int outside = ::open(context.outsideFile.c_str(), O_RDONLY | O_CLOEXEC);
            probe.outsideReadDenied = outside < 0 && (errno == EACCES || errno == EPERM);
            if (outside >= 0)
                ::close(outside);

            const int writeFd = ::open(context.allowedFile.c_str(), O_WRONLY | O_CLOEXEC);
            probe.pathWriteDenied = writeFd < 0 && (errno == EACCES || errno == EPERM);
            if (writeFd >= 0)
                ::close(writeFd);

            const std::string created = context.allowedDirectory + "/created-by-sandbox-test";
            const int createFd = ::open(created.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            probe.createDenied = createFd < 0 && (errno == EACCES || errno == EPERM);
            if (createFd >= 0) {
                ::close(createFd);
                ::unlink(created.c_str());
            }

            if (context.inheritedFd >= 0) {
                char byte = 0;
                probe.inheritedRead = ::pread(context.inheritedFd, &byte, 1, 0) == 1;
                probe.inheritedWrite = ::pwrite(context.inheritedFd, "x", 1, 0) == 1;
            }
        }

        // Report the filesystem and thread probes before exercising a syscall
        // that the strict allowlist intentionally terminates. The parent can
        // then verify both the probe result and the expected signal.
        if (status.seccomp) {
            try {
                std::atomic_bool ran { false };
                std::thread thread([&ran] { ran.store(true, std::memory_order_release); });
                thread.join();
                probe.threadAllowed = ran.load(std::memory_order_acquire);
            } catch (...) {
                probe.threadAllowed = false;
            }
        }

        const char marker = 'x';
        const ssize_t markerBytes = ::write(STDOUT_FILENO, &marker, 1);
        (void)markerBytes;
        const bool reported = writeProbeResult(pipeFds[1], probe);
        ::close(pipeFds[1]);
        _exit(reported ? 0 : 1);
    }

    ::close(pipeFds[1]);
    ::close(launcherPipe[1]);
    const bool received = readProbeResult(pipeFds[0], result);
    ::close(pipeFds[0]);
    int childStatus = 0;
    const bool waited = ::waitpid(pid, &childStatus, 0) == pid;
    char marker = 0;
    const ssize_t markerBytes = ::read(launcherPipe[0], &marker, 1);
    result->launcherPipeClosed = markerBytes == 0;
    ::close(launcherPipe[0]);
    if (!received || !waited)
        return false;
    return WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0;
}

bool forbiddenOperationIsKilled(const std::string& allowedDirectory, ForbiddenOperation operation)
{
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        const auto status = ::Mu::Worker::Sandbox::activate({ allowedDirectory });
        if (!status.seccomp)
            _exit(2);

        if (operation == ForbiddenOperation::Fork) {
            const pid_t child = ::fork();
            if (child == 0)
                _exit(0);
        } else if (operation == ForbiddenOperation::Exec) {
            const QString executable = QStandardPaths::findExecutable(QStringLiteral("true"));
            if (executable.isEmpty())
                _exit(3);
            char executablePath[4096];
            const QByteArray path = executable.toLocal8Bit();
            if (path.size() >= static_cast<int>(sizeof(executablePath)))
                _exit(3);
            std::memcpy(executablePath, path.constData(), static_cast<std::size_t>(path.size()) + 1);
            char* arguments[] = { executablePath, nullptr };
            char* environment[] = { nullptr };
            ::execve(executablePath, arguments, environment);
        } else if (operation == ForbiddenOperation::InternetSocket) {
            const int socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (socket >= 0)
                ::close(socket);
        } else {
            // Creating any AF_UNIX socket must also be killed, so the worker
            // cannot connect to the session bus, X11, or ssh-agent.
            const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (socket >= 0)
                ::close(socket);
        }
        _exit(1);
    }

    int childStatus = 0;
    if (::waitpid(pid, &childStatus, 0) != pid)
        return false;
    return WIFSIGNALED(childStatus);
}

} // namespace

class TestSandbox : public QObject {
    Q_OBJECT

private slots:
    void testFilesystemAndNetworkRestrictions();
    void testMissingOptionalDirectoryDoesNotDisableLandlock();
    void testMissingDefaultDirectoryKeepsLandlock();
};

void TestSandbox::testFilesystemAndNetworkRestrictions()
{
#ifndef __linux__
    QSKIP("Landlock is Linux-only");
#else
    QTemporaryDir allowedDir;
    QVERIFY(allowedDir.isValid());
    const QString allowedFile = allowedDir.filePath(QStringLiteral("allowed"));
    QFile file(allowedFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ok");
    file.close();

    QTemporaryDir outsideDir;
    QVERIFY(outsideDir.isValid());
    const QString outsideFile = outsideDir.filePath(QStringLiteral("outside"));
    QFile outside(outsideFile);
    QVERIFY(outside.open(QIODevice::WriteOnly));
    outside.write("outside");
    outside.close();

    const int inheritedFd = ::open(allowedFile.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    QVERIFY(inheritedFd >= 0);
    const int unpreservedFd = ::dup(inheritedFd);
    QVERIFY(unpreservedFd >= 0);
    const ProbeContext context {
        allowedDir.path().toStdString(),
        allowedFile.toStdString(),
        outsideFile.toStdString(),
        allowedDir.path().toStdString(),
        outsideDir.filePath(QStringLiteral("missing-optional")).toStdString(),
        inheritedFd,
        unpreservedFd,
    };
    ProbeResult result;
    QVERIFY(runSandboxProbe(context, &result));
    ::close(inheritedFd);
    ::close(unpreservedFd);

    if (!result.landlockActive && !result.seccompActive)
        QSKIP("Landlock and seccomp are unavailable on this host");

    QVERIFY(result.resourceLimitsActive);
#ifdef MU_DEBUG_ENABLED
    QVERIFY(!result.unpreservedClosed);
    QVERIFY(!result.launcherPipeClosed);
#else
    QVERIFY(result.unpreservedClosed);
    QVERIFY(result.launcherPipeClosed);
#endif

    if (result.linuxNamespaceActive) {
        QVERIFY(result.linuxNamespaceActive);
    }

    if (result.landlockActive) {
        QVERIFY(result.allowedRead);
        QVERIFY(result.outsideReadDenied);
        QVERIFY(result.pathWriteDenied);
        QVERIFY(result.createDenied);
        QVERIFY(result.inheritedRead);
        QVERIFY(result.inheritedWrite);
    }
    if (result.seccompActive) {
        QVERIFY(result.threadAllowed);
        QVERIFY(forbiddenOperationIsKilled(context.allowedDirectory, ForbiddenOperation::Fork));
        QVERIFY(forbiddenOperationIsKilled(context.allowedDirectory, ForbiddenOperation::Exec));
        QVERIFY(forbiddenOperationIsKilled(context.allowedDirectory, ForbiddenOperation::InternetSocket));
        QVERIFY(forbiddenOperationIsKilled(context.allowedDirectory, ForbiddenOperation::UnixSocket));
    }
#endif
}

void TestSandbox::testMissingOptionalDirectoryDoesNotDisableLandlock()
{
#ifndef __linux__
    QSKIP("Landlock is Linux-only");
#else
    QTemporaryDir requiredDir;
    QVERIFY(requiredDir.isValid());
    const QString requiredFile = requiredDir.filePath(QStringLiteral("required"));
    QFile file(requiredFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ok");
    file.close();

    const ProbeContext context { requiredDir.path().toStdString(),
                                 requiredFile.toStdString(),
                                 requiredFile.toStdString(),
                                 requiredDir.path().toStdString(),
                                 requiredDir.filePath(QStringLiteral("missing")).toStdString() };
    ProbeResult result;
    QVERIFY(runSandboxProbe(context, &result));
    QVERIFY(result.landlockActive);
#endif
}

void TestSandbox::testMissingDefaultDirectoryKeepsLandlock()
{
#ifndef __linux__
    QSKIP("Landlock is Linux-only");
#else
    QTemporaryDir optionalDir;
    QVERIFY(optionalDir.isValid());
    const QString optionalFile = optionalDir.filePath(QStringLiteral("optional"));
    QFile file(optionalFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("ok");
    file.close();

    const ProbeContext context { optionalDir.path().toStdString(),
                                 optionalFile.toStdString(),
                                 optionalFile.toStdString(),
                                 optionalDir.filePath(QStringLiteral("missing-default")).toStdString(),
                                 optionalDir.path().toStdString() };
    ProbeResult result;
    QVERIFY(runSandboxProbe(context, &result));
    QVERIFY(result.landlockActive);
#endif
}

QTEST_GUILESS_MAIN(TestSandbox)

#include "sandbox.moc"
