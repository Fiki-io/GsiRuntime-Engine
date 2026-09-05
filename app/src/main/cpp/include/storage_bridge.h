#pragma once

#include <string>
#include <mutex>
#include <vector>
#include <cstdint>

namespace gsi {

struct StorageStats {
    uint64_t totalBytes = 64ULL * 1024 * 1024 * 1024;    // 64 GB Virtual Capacity
    uint64_t freeBytes = 56ULL * 1024 * 1024 * 1024;     // ~56 GB Free
    uint64_t usedBytes = 8ULL * 1024 * 1024 * 1024;      // ~8 GB Used
    uint32_t directoryCount = 0;
    uint32_t fileCount = 0;
    bool isMounted = true;
};

class StorageBridge {
public:
    static StorageBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool populateStandardFolders();
    bool generateSampleFiles();
    bool wipeStorage();

    StorageStats getStats();
    std::string getStorageStatsString();
    void updateMountsFile();

private:
    StorageBridge() = default;
    ~StorageBridge() = default;

    void updatePropertyCache(const StorageStats& stats);
    void scanStorageMetrics(const std::string& dirPath, uint32_t& dirCount, uint32_t& fileCount, uint64_t& byteSize);

    std::mutex mStorageMutex;
    std::string mSandboxDir;
    std::string mMediaDir;
    bool mInitialized = false;
};

} // namespace gsi
