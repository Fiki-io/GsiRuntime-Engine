#pragma once

#include "init_parser.h"

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

namespace gsi {

class Ext4Reader;

enum class BootPhase {
    IDLE,
    EARLY_INIT,
    INIT,
    EARLY_BOOT,
    BOOT,
    COMPLETED
};

class BootManager {
public:
    static BootManager& getInstance();

    size_t loadInitFromVfs(Ext4Reader& reader);
    bool startBootSequence(const std::string& sandboxDir);
    void stopBootSequence();

    bool isBooting() const { return mIsBooting.load(); }
    BootPhase getPhase() const { return mPhase.load(); }
    std::string getPhaseString() const;

    std::vector<InitService> getServicesSnapshot() const;
    size_t getServicesCount() const;

    bool startService(const std::string& name);
    bool stopService(const std::string& name);
    bool restartService(const std::string& name);

    void setBootLogCallback(std::function<void(const std::string&)> cb) {
        mLogCallback = cb;
    }

private:
    BootManager() = default;
    ~BootManager();

    BootManager(const BootManager&) = delete;
    BootManager& operator=(const BootManager&) = delete;

    void bootWorker(std::string sandboxDir);
    void executeTriggerActions(const std::string& trigger, const std::string& sandboxDir);
    void logBoot(const std::string& msg);

    mutable std::mutex mMutex;
    InitParser mParser;
    std::atomic<BootPhase> mPhase{BootPhase::IDLE};
    std::atomic<bool> mIsBooting{false};
    std::thread mBootThread;

    std::function<void(const std::string&)> mLogCallback;
    int mNextPid{2000};
};

} // namespace gsi
