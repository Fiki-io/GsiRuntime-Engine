#include "include/boot_manager.h"
#include "include/property_service.h"
#include "include/logger.h"

#include <sys/stat.h>
#include <unistd.h>
#include <chrono>

namespace gsi {

namespace {

static bool createDir(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return (mkdir(path.c_str(), 0755) == 0);
}

} // anonymous namespace

BootManager& BootManager::getInstance() {
    static BootManager instance;
    return instance;
}

BootManager::~BootManager() {
    stopBootSequence();
}

size_t BootManager::loadInitFromVfs(Ext4Reader& reader) {
    std::lock_guard<std::mutex> lock(mMutex);
    return mParser.loadFromVfs(reader);
}

std::string BootManager::getPhaseString() const {
    switch (mPhase.load()) {
        case BootPhase::IDLE: return "IDLE";
        case BootPhase::EARLY_INIT: return "EARLY_INIT";
        case BootPhase::INIT: return "INIT";
        case BootPhase::EARLY_BOOT: return "EARLY_BOOT";
        case BootPhase::BOOT: return "BOOT";
        case BootPhase::COMPLETED: return "COMPLETED";
    }
    return "UNKNOWN";
}

std::vector<InitService> BootManager::getServicesSnapshot() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mParser.getServices();
}

size_t BootManager::getServicesCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mParser.getServices().size();
}

void BootManager::logBoot(const std::string& msg) {
    LOGI("%s", msg.c_str());
    if (mLogCallback) {
        mLogCallback(msg);
    }
}

bool BootManager::startBootSequence(const std::string& sandboxDir) {
    if (mIsBooting.load()) {
        LOGW("Boot sequence is already running");
        return true;
    }

    if (mBootThread.joinable()) {
        mBootThread.join();
    }

    mIsBooting.store(true);
    mPhase.store(BootPhase::IDLE);

    mBootThread = std::thread(&BootManager::bootWorker, this, sandboxDir);
    return true;
}

void BootManager::stopBootSequence() {
    mIsBooting.store(false);

    if (mBootThread.joinable()) {
        mBootThread.join();
    }

    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& s : mParser.getServicesMutable()) {
        if (s.status == "RUNNING") {
            s.status = "STOPPED";
            s.pid = -1;
            PropertyService::getInstance().setProperty("init.svc." + s.name, "stopped");
        }
    }

    mPhase.store(BootPhase::IDLE);
    logBoot("[init] Boot sequence stopped. All services reset to STOPPED.");
}

bool BootManager::startService(const std::string& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto* s = mParser.findServiceMutable(name);
    if (!s) return false;

    if (s->status == "RUNNING") return true;

    s->status = "RUNNING";
    s->pid = mNextPid++;
    PropertyService::getInstance().setProperty("init.svc." + s->name, "running");

    logBoot("[init] Service started: " + s->name + " (PID: " + std::to_string(s->pid) +
            ", User: " + s->user + ")");
    return true;
}

bool BootManager::stopService(const std::string& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto* s = mParser.findServiceMutable(name);
    if (!s) return false;

    s->status = "STOPPED";
    s->pid = -1;
    PropertyService::getInstance().setProperty("init.svc." + s->name, "stopped");

    logBoot("[init] Service stopped: " + s->name);
    return true;
}

bool BootManager::restartService(const std::string& name) {
    stopService(name);
    return startService(name);
}

void BootManager::executeTriggerActions(const std::string& trigger, const std::string& sandboxDir) {
    std::vector<std::string> commandsToRun;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const auto& a : mParser.getActions()) {
            if (a.trigger == trigger) {
                commandsToRun.insert(commandsToRun.end(), a.commands.begin(), a.commands.end());
            }
        }
    }

    for (const auto& cmd : commandsToRun) {
        if (cmd.rfind("mkdir ", 0) == 0) {
            std::string sub = cmd.substr(6);
            auto sp = sub.find(' ');
            std::string path = (sp != std::string::npos) ? sub.substr(0, sp) : sub;
            if (!path.empty() && path[0] == '/') {
                createDir(sandboxDir + path);
            }
        } else if (cmd.rfind("setprop ", 0) == 0) {
            std::string sub = cmd.substr(8);
            auto sp = sub.find(' ');
            if (sp != std::string::npos) {
                std::string k = sub.substr(0, sp);
                std::string v = sub.substr(sp + 1);
                PropertyService::getInstance().setProperty(k, v);
            }
        } else if (cmd.rfind("start ", 0) == 0) {
            std::string sName = cmd.substr(6);
            startService(sName);
        } else if (cmd.rfind("class_start ", 0) == 0) {
            std::string clsName = cmd.substr(12);
            std::lock_guard<std::mutex> lock(mMutex);
            for (auto& s : mParser.getServicesMutable()) {
                for (const auto& c : s.classes) {
                    if (c == clsName && s.status != "RUNNING" && !s.isDisabled) {
                        s.status = "RUNNING";
                        s.pid = mNextPid++;
                        PropertyService::getInstance().setProperty("init.svc." + s.name, "running");
                        logBoot("[init] class_start " + clsName + " -> Started: " + s.name);
                        break;
                    }
                }
            }
        }
    }
}

void BootManager::bootWorker(std::string sandboxDir) {
    logBoot("=== STARTING ANDROID GSI INIT BOOT SEQUENCE ===");

    // Phase 1: early-init
    mPhase.store(BootPhase::EARLY_INIT);
    logBoot("[init] [Phase 1/4] Trigger: on early-init");
    createDir(sandboxDir + "/dev");
    createDir(sandboxDir + "/dev/socket");
    createDir(sandboxDir + "/proc");
    createDir(sandboxDir + "/sys");
    createDir(sandboxDir + "/system");
    createDir(sandboxDir + "/data");
    executeTriggerActions("early-init", sandboxDir);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (!mIsBooting.load()) return;

    // Phase 2: init
    mPhase.store(BootPhase::INIT);
    logBoot("[init] [Phase 2/4] Trigger: on init (Starting Property Service & Core Daemons)");
    PropertyService::getInstance().start(sandboxDir);

    // Start class core services (servicemanager, logd, etc.)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& s : mParser.getServicesMutable()) {
            for (const auto& c : s.classes) {
                if (c == "core" && s.status != "RUNNING" && !s.isDisabled) {
                    s.status = "RUNNING";
                    s.pid = mNextPid++;
                    PropertyService::getInstance().setProperty("init.svc." + s.name, "running");
                    logBoot("[init] Core Daemon started: " + s.name + " (PID: " + std::to_string(s.pid) + ")");
                    break;
                }
            }
        }
    }
    executeTriggerActions("init", sandboxDir);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (!mIsBooting.load()) return;

    // Phase 3: early-boot & boot
    mPhase.store(BootPhase::EARLY_BOOT);
    logBoot("[init] [Phase 3/4] Trigger: on early-boot");
    executeTriggerActions("early-boot", sandboxDir);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    mPhase.store(BootPhase::BOOT);
    logBoot("[init] [Phase 3/4] Trigger: on boot (Starting Main Framework Daemons)");

    // Start class main services (surfaceflinger, vold, etc.)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& s : mParser.getServicesMutable()) {
            for (const auto& c : s.classes) {
                if ((c == "main" || c == "animation") && s.status != "RUNNING" && !s.isDisabled) {
                    s.status = "RUNNING";
                    s.pid = mNextPid++;
                    PropertyService::getInstance().setProperty("init.svc." + s.name, "running");
                    logBoot("[init] Framework Daemon started: " + s.name + " (PID: " + std::to_string(s.pid) + ")");
                    break;
                }
            }
        }
    }
    executeTriggerActions("boot", sandboxDir);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (!mIsBooting.load()) return;

    // Phase 4: boot_completed
    mPhase.store(BootPhase::COMPLETED);
    PropertyService::getInstance().setProperty("sys.boot_completed", "1");
    PropertyService::getInstance().setProperty("service.bootanim.exit", "1");
    PropertyService::getInstance().setProperty("dev.bootcomplete", "1");

    logBoot("[init] [Phase 4/4] Trigger: sys.boot_completed=1 (Android Framework Ready!)");
    logBoot("=== ANDROID GSI BOOT SEQUENCE COMPLETED SUCCESSFULLY ===");

    mIsBooting.store(false);
}

} // namespace gsi
