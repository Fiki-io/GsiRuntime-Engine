#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <sys/types.h>

namespace gsi {

class ProcessSpawner {
public:
    static ProcessSpawner& getInstance();

    bool execute(const std::string& command, const std::string& sandboxDir);
    std::string pollOutput();
    bool sendInput(const std::string& input);
    bool isRunning();
    void terminate();
    int getExitCode() const { return mExitCode.load(); }

private:
    ProcessSpawner();
    ~ProcessSpawner();

    void ioLoop();
    void cleanupPipes();

    std::atomic<pid_t> mChildPid{-1};
    int mStdinPipe[2]{-1, -1};
    int mStdoutPipe[2]{-1, -1};
    int mStderrPipe[2]{-1, -1};

    std::thread mIoThread;
    std::mutex mOutputMutex;
    std::string mOutputBuffer;

    std::atomic<bool> mIsRunning{false};
    std::atomic<int> mExitCode{0};
};

} // namespace gsi
