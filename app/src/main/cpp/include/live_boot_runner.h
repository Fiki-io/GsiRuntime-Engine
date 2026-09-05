#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

namespace gsi {

struct LiveBootResult {
    bool success = false;
    std::string osVersion;
    std::string sdkVersion;
    std::string buildId;
    std::string model;
    size_t totalServices = 0;
    size_t totalProperties = 0;
    std::string bootLog;
};

class LiveBootRunner {
public:
    static LiveBootRunner& getInstance();

    LiveBootResult bootFromPath(const std::string& imagePath, const std::string& sandboxDir);
    LiveBootResult bootFromFd(int fd, const std::string& sandboxDir);

    std::string getLastBootLog() const;
    bool isBootActive() const { return mBootActive.load(); }

    void setLogCallback(std::function<void(const std::string&)> cb) {
        mLogCallback = cb;
    }

private:
    LiveBootRunner() = default;
    ~LiveBootRunner() = default;

    void appendLog(const std::string& line);

    mutable std::mutex mMutex;
    std::atomic<bool> mBootActive{false};
    std::string mAccumulatedLog;
    std::function<void(const std::string&)> mLogCallback;
};

} // namespace gsi
