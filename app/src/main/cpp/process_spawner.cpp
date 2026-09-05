#include "include/process_spawner.h"
#include "include/logger.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <cstring>
#include <sstream>

namespace gsi {

ProcessSpawner& ProcessSpawner::getInstance() {
    static ProcessSpawner instance;
    return instance;
}

ProcessSpawner::ProcessSpawner() = default;

ProcessSpawner::~ProcessSpawner() {
    terminate();
}

void ProcessSpawner::cleanupPipes() {
    if (mStdinPipe[0] != -1) { close(mStdinPipe[0]); mStdinPipe[0] = -1; }
    if (mStdinPipe[1] != -1) { close(mStdinPipe[1]); mStdinPipe[1] = -1; }
    if (mStdoutPipe[0] != -1) { close(mStdoutPipe[0]); mStdoutPipe[0] = -1; }
    if (mStdoutPipe[1] != -1) { close(mStdoutPipe[1]); mStdoutPipe[1] = -1; }
    if (mStderrPipe[0] != -1) { close(mStderrPipe[0]); mStderrPipe[0] = -1; }
    if (mStderrPipe[1] != -1) { close(mStderrPipe[1]); mStderrPipe[1] = -1; }
}

bool ProcessSpawner::execute(const std::string& command, const std::string& sandboxDir) {
    if (mIsRunning.load()) {
        LOGW("A process is already running. Terminating previous process...");
        terminate();
    }

    if (pipe2(mStdinPipe, O_CLOEXEC) < 0 ||
        pipe2(mStdoutPipe, O_CLOEXEC) < 0 ||
        pipe2(mStderrPipe, O_CLOEXEC) < 0) {
        LOGE("Failed to create UNIX pipes for process spawner");
        cleanupPipes();
        return false;
    }

    // Determine shell: prefer sandbox shell if exists, else host system shell
    std::string shellPath = sandboxDir + "/bin/sh";
    if (access(shellPath.c_str(), X_OK) != 0) {
        shellPath = "/system/bin/sh";
    }

    LOGI("Spawning process via: %s -c \"%s\"", shellPath.c_str(), command.c_str());

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork() failed");
        cleanupPipes();
        return false;
    }

    if (pid == 0) {
        // --- Child Process ---
        setsid();

        // Redirect stdin, stdout, stderr
        dup2(mStdinPipe[0], STDIN_FILENO);
        dup2(mStdoutPipe[1], STDOUT_FILENO);
        dup2(mStderrPipe[1], STDERR_FILENO);

        // Close pipe descriptors in child
        close(mStdinPipe[0]);
        close(mStdinPipe[1]);
        close(mStdoutPipe[0]);
        close(mStdoutPipe[1]);
        close(mStderrPipe[0]);
        close(mStderrPipe[1]);

        // Configure GSI Sandbox Environment
        setenv("ANDROID_ROOT", "/system", 1);
        setenv("ANDROID_DATA", (sandboxDir + "/data").c_str(), 1);
        std::string pathEnv = sandboxDir + "/bin:/system/bin:/system/xbin:/vendor/bin";
        setenv("PATH", pathEnv.c_str(), 1);

        std::string ldEnv = sandboxDir + "/lib64:/system/lib64:/vendor/lib64";
        setenv("LD_LIBRARY_PATH", ldEnv.c_str(), 1);

        // Inject in-process hook library via LD_PRELOAD if available
        std::string hookLib = sandboxDir + "/lib64/libgsi_hook.so";
        if (access(hookLib.c_str(), R_OK) == 0) {
            setenv("LD_PRELOAD", hookLib.c_str(), 1);
        }

        // Virtual identity & GSI sandbox configuration
        setenv("GSI_SANDBOX_DIR", sandboxDir.c_str(), 1);
        setenv("GSI_VIRTUAL_UID", "0", 1);
        setenv("GSI_VIRTUAL_GID", "0", 1);
        setenv("USER", "root", 1);
        setenv("USERNAME", "root", 1);
        setenv("ANDROID_PROPERTY_WORKSPACE", (sandboxDir + "/dev/__properties_text__").c_str(), 1);

        setenv("TMPDIR", (sandboxDir + "/tmp").c_str(), 1);
        setenv("HOME", (sandboxDir + "/data").c_str(), 1);

        // Change current directory to sandbox
        chdir(sandboxDir.c_str());

        char* const argv[] = {
            const_cast<char*>(shellPath.c_str()),
            const_cast<char*>("-c"),
            const_cast<char*>(command.c_str()),
            nullptr
        };

        execve(shellPath.c_str(), argv, environ);

        // If execve fails
        const char* err = "Failed to execute shell in child process\n";
        write(STDERR_FILENO, err, strlen(err));
        _exit(127);
    }

    // --- Parent Process ---
    mChildPid.store(pid);
    mIsRunning.store(true);
    mExitCode.store(0);

    // Close child-side pipe ends in parent
    close(mStdinPipe[0]);  mStdinPipe[0] = -1;
    close(mStdoutPipe[1]); mStdoutPipe[1] = -1;
    close(mStderrPipe[1]); mStderrPipe[1] = -1;

    // Start background reader thread
    if (mIoThread.joinable()) {
        mIoThread.join();
    }
    mIoThread = std::thread(&ProcessSpawner::ioLoop, this);

    LOGI("Child process spawned with PID: %d", pid);
    return true;
}

void ProcessSpawner::ioLoop() {
    char buf[2048];
    struct pollfd fds[2];

    fds[0].fd = mStdoutPipe[0];
    fds[0].events = POLLIN;
    fds[1].fd = mStderrPipe[0];
    fds[1].events = POLLIN;

    while (mIsRunning.load()) {
        int pollRet = poll(fds, 2, 100);

        if (pollRet > 0) {
            // Read stdout
            if (fds[0].revents & POLLIN) {
                ssize_t bytes = read(mStdoutPipe[0], buf, sizeof(buf) - 1);
                if (bytes > 0) {
                    buf[bytes] = '\0';
                    std::lock_guard<std::mutex> lock(mOutputMutex);
                    mOutputBuffer.append(buf, bytes);
                }
            }

            // Read stderr
            if (fds[1].revents & POLLIN) {
                ssize_t bytes = read(mStderrPipe[0], buf, sizeof(buf) - 1);
                if (bytes > 0) {
                    buf[bytes] = '\0';
                    std::lock_guard<std::mutex> lock(mOutputMutex);
                    mOutputBuffer.append(buf, bytes);
                }
            }

            // Check EOF / Hangup
            if ((fds[0].revents & (POLLHUP | POLLERR)) &&
                (fds[1].revents & (POLLHUP | POLLERR))) {
                break;
            }
        }

        // Check if process finished
        pid_t pid = mChildPid.load();
        if (pid > 0) {
            int status = 0;
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid) {
                if (WIFEXITED(status)) {
                    mExitCode.store(WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    mExitCode.store(128 + WTERMSIG(status));
                }
                break;
            }
        }
    }

    // Drain any remaining output
    if (mStdoutPipe[0] != -1) {
        ssize_t bytes;
        while ((bytes = read(mStdoutPipe[0], buf, sizeof(buf) - 1)) > 0) {
            buf[bytes] = '\0';
            std::lock_guard<std::mutex> lock(mOutputMutex);
            mOutputBuffer.append(buf, bytes);
        }
    }
    if (mStderrPipe[0] != -1) {
        ssize_t bytes;
        while ((bytes = read(mStderrPipe[0], buf, sizeof(buf) - 1)) > 0) {
            buf[bytes] = '\0';
            std::lock_guard<std::mutex> lock(mOutputMutex);
            mOutputBuffer.append(buf, bytes);
        }
    }

    mIsRunning.store(false);
    LOGI("Child process %d exited with code: %d", mChildPid.load(), mExitCode.load());
}

std::string ProcessSpawner::pollOutput() {
    std::lock_guard<std::mutex> lock(mOutputMutex);
    std::string out = std::move(mOutputBuffer);
    mOutputBuffer.clear();
    return out;
}

bool ProcessSpawner::sendInput(const std::string& input) {
    if (mStdinPipe[1] == -1 || !mIsRunning.load()) {
        return false;
    }
    ssize_t written = write(mStdinPipe[1], input.data(), input.size());
    return (written == static_cast<ssize_t>(input.size()));
}

bool ProcessSpawner::isRunning() {
    if (!mIsRunning.load()) return false;
    pid_t pid = mChildPid.load();
    if (pid <= 0) return false;

    int status = 0;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
        mIsRunning.store(false);
        if (WIFEXITED(status)) mExitCode.store(WEXITSTATUS(status));
        return false;
    }
    return true;
}

void ProcessSpawner::terminate() {
    pid_t pid = mChildPid.load();
    if (pid > 0) {
        kill(pid, SIGTERM);
        usleep(50000); // 50ms grace period
        kill(pid, SIGKILL);

        int status = 0;
        waitpid(pid, &status, 0);
        mChildPid.store(-1);
    }

    mIsRunning.store(false);

    if (mIoThread.joinable()) {
        mIoThread.join();
    }

    cleanupPipes();
    LOGI("Process terminated and resources cleaned up");
}

} // namespace gsi
