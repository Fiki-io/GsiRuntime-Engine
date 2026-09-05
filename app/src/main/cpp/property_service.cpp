#include "include/property_service.h"
#include "include/logger.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace gsi {

namespace {

#define PROP_MSG_SETPROP 1
#define PROP_MSG_SETPROP2 0x00020001
#define PROP_SUCCESS 0

struct LegacyPropMsg {
    uint32_t cmd;
    char name[32];
    char value[92];
};

static bool ensureDirExists(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return (mkdir(path.c_str(), 0755) == 0);
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // anonymous namespace

PropertyService& PropertyService::getInstance() {
    static PropertyService instance;
    return instance;
}

PropertyService::PropertyService() = default;

PropertyService::~PropertyService() {
    stop();
}

bool PropertyService::start(const std::string& sandboxDir) {
    if (mIsRunning.load()) {
        LOGW("PropertyService is already running.");
        return true;
    }

    mSandboxDir = sandboxDir;
    std::string devDir = sandboxDir + "/dev";
    std::string socketDir = devDir + "/socket";
    std::string procDir = sandboxDir + "/proc";
    std::string binDir = sandboxDir + "/bin";

    ensureDirExists(devDir);
    ensureDirExists(socketDir);
    ensureDirExists(procDir);
    ensureDirExists(binDir);

    mSocketPath = socketDir + "/property_service";
    mPropertiesFilePath = devDir + "/__properties_text__";

    // 1. Generate virtual /proc/cmdline for Android init and bootstrap daemons
    std::string cmdlinePath = procDir + "/cmdline";
    int cmdFd = open(cmdlinePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cmdFd >= 0) {
        const char* cmdline = "androidboot.hardware=gsi androidboot.selinux=permissive "
                              "androidboot.mode=normal ro.build.type=userdebug console=null\n";
        write(cmdFd, cmdline, strlen(cmdline));
        close(cmdFd);
    }

    // 2. Generate helper shell scripts for getprop and setprop in $SANDBOX/bin
    std::string getpropScript = binDir + "/getprop";
    int gpFd = open(getpropScript.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (gpFd >= 0) {
        std::string script =
            "#!/system/bin/sh\n"
            "PROP_FILE=\"" + mPropertiesFilePath + "\"\n"
            "if [ -z \"$1\" ]; then\n"
            "    if [ -f \"$PROP_FILE\" ]; then\n"
            "        cat \"$PROP_FILE\"\n"
            "    fi\n"
            "else\n"
            "    KEY=\"$1\"\n"
            "    VAL=$(grep \"^\\[$KEY\\]: \" \"$PROP_FILE\" 2>/dev/null | sed \"s/^\\[$KEY\\]: \\[\\(.*\\)\\]/\\1/\")\n"
            "    if [ -n \"$VAL\" ]; then\n"
            "        echo \"$VAL\"\n"
            "    else\n"
            "        echo \"\"\n"
            "    fi\n"
            "fi\n";
        write(gpFd, script.c_str(), script.size());
        close(gpFd);
        chmod(getpropScript.c_str(), 0755);
    }

    std::string setpropScript = binDir + "/setprop";
    int spFd = open(setpropScript.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (spFd >= 0) {
        std::string script =
            "#!/system/bin/sh\n"
            "if [ -z \"$1\" ] || [ -z \"$2\" ]; then\n"
            "    echo \"Usage: setprop <key> <value>\"\n"
            "    exit 1\n"
            "fi\n"
            "SOCK=\"" + mSocketPath + "\"\n"
            "echo \"$1=$2\" | toybox nc -U \"$SOCK\" 2>/dev/null || true\n";
        write(spFd, script.c_str(), script.size());
        close(spFd);
        chmod(setpropScript.c_str(), 0755);
    }

    // 3. Unlink existing socket file
    unlink(mSocketPath.c_str());

    // 4. Create UNIX domain stream socket
    mServerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mServerFd < 0) {
        LOGE("PropertyService: Failed to create AF_UNIX socket");
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, mSocketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(mServerFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("PropertyService: Failed to bind socket to %s", mSocketPath.c_str());
        close(mServerFd);
        mServerFd = -1;
        return false;
    }

    chmod(mSocketPath.c_str(), 0666);

    if (listen(mServerFd, 16) < 0) {
        LOGE("PropertyService: Failed to listen on socket");
        close(mServerFd);
        mServerFd = -1;
        return false;
    }

    // Ensure default system properties
    {
        std::lock_guard<std::mutex> lock(mPropMutex);
        if (mProperties.find("ro.debuggable") == mProperties.end()) {
            mProperties["ro.debuggable"] = "1";
            mProperties["ro.secure"] = "0";
            mProperties["ro.boot.mode"] = "normal";
            mProperties["sys.boot_completed"] = "1";
            mProperties["service.bootanim.exit"] = "1";
            mProperties["init.svc.servicemanager"] = "running";
            mProperties["init.svc.surfaceflinger"] = "running";
            mProperties["init.svc.zygote"] = "running";
            mProperties["ro.build.type"] = "userdebug";
            mProperties["persist.sys.timezone"] = "UTC";
            mProperties["vold.decrypt"] = "trigger_restart_framework";
        }
    }

    flushPropertiesToFile();

    mIsRunning.store(true);
    mWorkerThread = std::thread(&PropertyService::workerLoop, this);

    LOGI("PropertyService started successfully. Listening at %s", mSocketPath.c_str());
    return true;
}

void PropertyService::stop() {
    if (!mIsRunning.load()) return;

    mIsRunning.store(false);

    if (mServerFd != -1) {
        shutdown(mServerFd, SHUT_RDWR);
        close(mServerFd);
        mServerFd = -1;
    }

    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }

    if (!mSocketPath.empty()) {
        unlink(mSocketPath.c_str());
    }

    LOGI("PropertyService stopped and unlinked.");
}

void PropertyService::loadFromBuildProp(const std::string& buildPropContent) {
    std::lock_guard<std::mutex> lock(mPropMutex);

    std::istringstream stream(buildPropContent);
    std::string line;
    size_t loadedCount = 0;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        auto eqPos = trimmed.find('=');
        if (eqPos != std::string::npos) {
            std::string key = trim(trimmed.substr(0, eqPos));
            std::string val = trim(trimmed.substr(eqPos + 1));
            if (!key.empty()) {
                mProperties[key] = val;
                loadedCount++;
            }
        }
    }

    // Default runtime flags if not in build.prop
    if (mProperties.find("ro.debuggable") == mProperties.end()) mProperties["ro.debuggable"] = "1";
    if (mProperties.find("ro.secure") == mProperties.end()) mProperties["ro.secure"] = "0";
    if (mProperties.find("ro.boot.mode") == mProperties.end()) mProperties["ro.boot.mode"] = "normal";
    if (mProperties.find("sys.boot_completed") == mProperties.end()) mProperties["sys.boot_completed"] = "1";
    if (mProperties.find("service.bootanim.exit") == mProperties.end()) mProperties["service.bootanim.exit"] = "1";
    if (mProperties.find("init.svc.servicemanager") == mProperties.end()) mProperties["init.svc.servicemanager"] = "running";
    if (mProperties.find("init.svc.surfaceflinger") == mProperties.end()) mProperties["init.svc.surfaceflinger"] = "running";
    if (mProperties.find("init.svc.zygote") == mProperties.end()) mProperties["init.svc.zygote"] = "running";

    LOGI("PropertyService: Loaded %zu properties from build.prop", loadedCount);
    flushPropertiesToFile();
}

void PropertyService::setProperty(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(mPropMutex);
        mProperties[key] = value;
    }
    LOGI("PropertyService: setprop [%s] = [%s]", key.c_str(), value.c_str());
    flushPropertiesToFile();
}

std::string PropertyService::getProperty(const std::string& key, const std::string& defaultValue) const {
    std::lock_guard<std::mutex> lock(mPropMutex);
    auto it = mProperties.find(key);
    if (it != mProperties.end()) {
        return it->second;
    }
    return defaultValue;
}

std::string PropertyService::getAllPropertiesFormatted() const {
    std::lock_guard<std::mutex> lock(mPropMutex);
    std::ostringstream oss;
    std::vector<std::pair<std::string, std::string>> sorted(mProperties.begin(), mProperties.end());
    std::sort(sorted.begin(), sorted.end());

    for (const auto& [k, v] : sorted) {
        oss << "[" << k << "]: [" << v << "]\n";
    }
    return oss.str();
}

size_t PropertyService::getPropertyCount() const {
    std::lock_guard<std::mutex> lock(mPropMutex);
    return mProperties.size();
}

void PropertyService::flushPropertiesToFile() {
    if (mPropertiesFilePath.empty()) return;

    std::ostringstream oss;
    {
        std::vector<std::pair<std::string, std::string>> sorted(mProperties.begin(), mProperties.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& [k, v] : sorted) {
            oss << "[" << k << "]: [" << v << "]\n";
        }
    }

    std::string content = oss.str();
    int fd = open(mPropertiesFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, content.data(), content.size());
        close(fd);
    }
}

void PropertyService::workerLoop() {
    struct pollfd pfd{};
    pfd.fd = mServerFd;
    pfd.events = POLLIN;

    while (mIsRunning.load()) {
        int ret = poll(&pfd, 1, 300);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_un clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(mServerFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
            if (clientFd >= 0) {
                handleClient(clientFd);
                close(clientFd);
            }
        }
    }
}

void PropertyService::handleClient(int clientFd) {
    char buf[1024];
    ssize_t n = read(clientFd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    // 1. Check if Bionic legacy format: sizeof(LegacyPropMsg) = 128 bytes
    if (n >= static_cast<ssize_t>(sizeof(LegacyPropMsg))) {
        auto* msg = reinterpret_cast<LegacyPropMsg*>(buf);
        if (msg->cmd == PROP_MSG_SETPROP) {
            std::string key(msg->name, strnlen(msg->name, sizeof(msg->name)));
            std::string val(msg->value, strnlen(msg->value, sizeof(msg->value)));
            if (!key.empty()) {
                setProperty(key, val);
            }
            uint32_t resp = PROP_SUCCESS;
            write(clientFd, &resp, sizeof(resp));
            return;
        }
    }

    // 2. Check if modern Bionic format: cmd == 0x00020001
    uint32_t cmd = 0;
    std::memcpy(&cmd, buf, sizeof(cmd));
    if (cmd == PROP_MSG_SETPROP2 && n > 8) {
        // [cmd: 4B][name_len: 4B][name...][val_len: 4B][val...]
        uint32_t nameLen = 0;
        std::memcpy(&nameLen, buf + 4, sizeof(nameLen));
        if (8 + nameLen + 4 <= static_cast<size_t>(n)) {
            std::string key(buf + 8, nameLen);
            uint32_t valLen = 0;
            std::memcpy(&valLen, buf + 8 + nameLen, sizeof(valLen));
            if (8 + nameLen + 4 + valLen <= static_cast<size_t>(n)) {
                std::string val(buf + 8 + nameLen + 4, valLen);
                setProperty(key, val);
                uint32_t resp = PROP_SUCCESS;
                write(clientFd, &resp, sizeof(resp));
                return;
            }
        }
    }

    // 3. Plain text format: "key=val" or "setprop key val" or "getprop key"
    std::string text(buf, n);
    std::string trimmed = trim(text);

    if (trimmed.rfind("setprop ", 0) == 0) {
        trimmed = trim(trimmed.substr(8));
        auto sp = trimmed.find(' ');
        if (sp != std::string::npos) {
            std::string key = trim(trimmed.substr(0, sp));
            std::string val = trim(trimmed.substr(sp + 1));
            setProperty(key, val);
            uint32_t resp = PROP_SUCCESS;
            write(clientFd, &resp, sizeof(resp));
            return;
        }
    } else if (trimmed.rfind("getprop ", 0) == 0) {
        std::string key = trim(trimmed.substr(8));
        std::string val = getProperty(key);
        val += "\n";
        write(clientFd, val.data(), val.size());
        return;
    } else {
        auto eq = trimmed.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(trimmed.substr(0, eq));
            std::string val = trim(trimmed.substr(eq + 1));
            if (!key.empty()) {
                setProperty(key, val);
                uint32_t resp = PROP_SUCCESS;
                write(clientFd, &resp, sizeof(resp));
                return;
            }
        }
    }

    uint32_t resp = PROP_SUCCESS;
    write(clientFd, &resp, sizeof(resp));
}

} // namespace gsi
