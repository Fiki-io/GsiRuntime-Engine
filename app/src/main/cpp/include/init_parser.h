#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace gsi {

class Ext4Reader;

struct InitAction {
    std::string trigger; // e.g. "early-init", "init", "early-boot", "boot"
    std::vector<std::string> commands; // e.g. "mkdir /dev/socket 0755", "setprop ...", "start servicemanager"
};

struct InitService {
    std::string name;             // e.g. "servicemanager", "surfaceflinger", "logd"
    std::string binaryPath;       // e.g. "/system/bin/servicemanager"
    std::vector<std::string> args;
    std::vector<std::string> classes; // e.g. "core", "main", "late_start"
    std::string user = "root";    // e.g. "system", "root", "logd"
    std::vector<std::string> groups;
    bool isCritical = false;
    bool isDisabled = false;
    std::vector<std::pair<std::string, std::string>> envVars;
    std::string status = "STOPPED"; // "STOPPED", "RUNNING", "EXITED"
    int pid = -1;
};

class InitParser {
public:
    InitParser() = default;
    ~InitParser() = default;

    void parseScriptContent(const std::string& content, const std::string& filename = "");
    size_t loadFromVfs(Ext4Reader& reader);

    const std::vector<InitAction>& getActions() const { return mActions; }
    const std::vector<InitService>& getServices() const { return mServices; }
    std::vector<InitService>& getServicesMutable() { return mServices; }

    const InitService* findService(const std::string& name) const;
    InitService* findServiceMutable(const std::string& name);

    void clear();

private:
    std::vector<InitAction> mActions;
    std::vector<InitService> mServices;
    std::unordered_map<std::string, size_t> mServiceIndexMap;
};

} // namespace gsi
