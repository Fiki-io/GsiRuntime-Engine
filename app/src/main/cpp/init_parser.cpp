#include "include/init_parser.h"
#include "include/ext4_reader.h"
#include "include/logger.h"

#include <sstream>
#include <algorithm>

namespace gsi {

namespace {

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> splitTokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

} // anonymous namespace

void InitParser::clear() {
    mActions.clear();
    mServices.clear();
    mServiceIndexMap.clear();
}

const InitService* InitParser::findService(const std::string& name) const {
    auto it = mServiceIndexMap.find(name);
    if (it != mServiceIndexMap.end() && it->second < mServices.size()) {
        return &mServices[it->second];
    }
    return nullptr;
}

InitService* InitParser::findServiceMutable(const std::string& name) {
    auto it = mServiceIndexMap.find(name);
    if (it != mServiceIndexMap.end() && it->second < mServices.size()) {
        return &mServices[it->second];
    }
    return nullptr;
}

void InitParser::parseScriptContent(const std::string& content, const std::string& filename) {
    std::istringstream stream(content);
    std::string rawLine;

    enum class SectionType { NONE, ACTION, SERVICE };
    SectionType currentSection = SectionType::NONE;

    InitAction currentAction;
    InitService currentService;

    auto finishCurrentSection = [&]() {
        if (currentSection == SectionType::ACTION) {
            if (!currentAction.trigger.empty()) {
                mActions.push_back(std::move(currentAction));
            }
            currentAction = InitAction();
        } else if (currentSection == SectionType::SERVICE) {
            if (!currentService.name.empty()) {
                if (mServiceIndexMap.find(currentService.name) == mServiceIndexMap.end()) {
                    mServiceIndexMap[currentService.name] = mServices.size();
                    mServices.push_back(std::move(currentService));
                }
            }
            currentService = InitService();
        }
        currentSection = SectionType::NONE;
    };

    while (std::getline(stream, rawLine)) {
        std::string trimmed = trim(rawLine);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        std::vector<std::string> tokens = splitTokens(trimmed);
        if (tokens.empty()) continue;

        const std::string& first = tokens[0];

        // 1. Check for Action header: "on <trigger>"
        if (first == "on" && tokens.size() >= 2) {
            finishCurrentSection();
            currentSection = SectionType::ACTION;
            currentAction.trigger = tokens[1];
            continue;
        }

        // 2. Check for Service header: "service <name> <path> [args...]"
        if (first == "service" && tokens.size() >= 3) {
            finishCurrentSection();
            currentSection = SectionType::SERVICE;
            currentService.name = tokens[1];
            currentService.binaryPath = tokens[2];
            for (size_t i = 3; i < tokens.size(); ++i) {
                currentService.args.push_back(tokens[i]);
            }
            // Default class is "main" if not specified
            currentService.classes.push_back("main");
            continue;
        }

        // 3. Process directives inside Action section
        if (currentSection == SectionType::ACTION) {
            currentAction.commands.push_back(trimmed);
            continue;
        }

        // 4. Process directives inside Service section
        if (currentSection == SectionType::SERVICE) {
            if (first == "class" && tokens.size() >= 2) {
                currentService.classes.clear();
                for (size_t i = 1; i < tokens.size(); ++i) {
                    currentService.classes.push_back(tokens[i]);
                }
            } else if (first == "user" && tokens.size() >= 2) {
                currentService.user = tokens[1];
            } else if (first == "group" && tokens.size() >= 2) {
                for (size_t i = 1; i < tokens.size(); ++i) {
                    currentService.groups.push_back(tokens[i]);
                }
            } else if (first == "critical") {
                currentService.isCritical = true;
            } else if (first == "disabled") {
                currentService.isDisabled = true;
            } else if (first == "setenv" && tokens.size() >= 3) {
                currentService.envVars.emplace_back(tokens[1], tokens[2]);
            }
            continue;
        }
    }

    finishCurrentSection();

    LOGI("InitParser: Parsed %s -> Total Actions: %zu, Total Services: %zu",
         filename.empty() ? "in-memory script" : filename.c_str(),
         mActions.size(), mServices.size());
}

size_t InitParser::loadFromVfs(Ext4Reader& reader) {
    clear();

    // 1. Try reading root /init.rc
    std::string rootInitContent;
    if (reader.readFileString("/init.rc", rootInitContent)) {
        LOGI("InitParser: Found /init.rc in VFS (%zu bytes)", rootInitContent.size());
        parseScriptContent(rootInitContent, "/init.rc");
    }

    // 2. Scan /system/etc/init
    std::vector<FsFileEntry> initDirEntries;
    if (reader.listDirectory("/system/etc/init", initDirEntries)) {
        LOGI("InitParser: Scanning /system/etc/init (%zu entries)", initDirEntries.size());
        for (const auto& entry : initDirEntries) {
            if (!entry.isDirectory && entry.name.size() > 3 &&
                entry.name.substr(entry.name.size() - 3) == ".rc") {
                std::string scriptContent;
                if (reader.readFileString(entry.path, scriptContent)) {
                    parseScriptContent(scriptContent, entry.path);
                }
            }
        }
    }

    // 3. Fallback: ensure vital core services exist if not defined in .rc
    auto ensureCoreService = [&](const std::string& name, const std::string& path,
                                 const std::string& cls, const std::string& user) {
        if (mServiceIndexMap.find(name) == mServiceIndexMap.end()) {
            InitService s;
            s.name = name;
            s.binaryPath = path;
            s.classes = {cls};
            s.user = user;
            s.status = "STOPPED";
            mServiceIndexMap[name] = mServices.size();
            mServices.push_back(s);
            LOGI("InitParser: Registered default core service: %s (%s)", name.c_str(), path.c_str());
        }
    };

    ensureCoreService("servicemanager", "/system/bin/servicemanager", "core", "system");
    ensureCoreService("surfaceflinger", "/system/bin/surfaceflinger", "core", "system");
    ensureCoreService("logd", "/system/bin/logd", "core", "logd");
    ensureCoreService("vold", "/system/bin/vold", "core", "root");
    ensureCoreService("sh", "/system/bin/sh", "main", "root");

    LOGI("InitParser: Successfully loaded %zu services and %zu actions from VFS",
         mServices.size(), mActions.size());

    return mServices.size();
}

} // namespace gsi
