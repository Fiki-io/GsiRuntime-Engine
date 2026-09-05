#pragma once

#include "ext4_reader.h"
#include <string>
#include <vector>

namespace gsi {

class SandboxManager {
public:
    static SandboxManager& getInstance();

    bool initializeSandbox(const std::string& basePath);
    bool deployHookLibrary(const std::string& hostLibDir);
    bool extractBinaryFromVfs(Ext4Reader& reader, const std::string& vfsPath, const std::string& targetName);

    std::string getBinDir() const { return mBinDir; }
    std::string getLibDir() const { return mLibDir; }
    std::string getDataDir() const { return mDataDir; }
    std::string getTmpDir() const { return mTmpDir; }
    std::string getBasePath() const { return mBasePath; }

private:
    SandboxManager() = default;

    std::string mBasePath;
    std::string mBinDir;
    std::string mLibDir;
    std::string mDataDir;
    std::string mTmpDir;
};

} // namespace gsi
