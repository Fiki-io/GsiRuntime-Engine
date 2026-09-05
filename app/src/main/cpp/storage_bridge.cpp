#include "include/storage_bridge.h"
#include "include/property_service.h"

#include <android/log.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>

#define LOG_TAG "GSI_StorageBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace gsi {

static void createDirRecursive(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            mkdir(current.c_str(), 0775);
        }
    }
}

static void removeDirRecursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string subPath = path + "/" + entry->d_name;
        struct stat st;
        if (lstat(subPath.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                removeDirRecursive(subPath);
                rmdir(subPath.c_str());
            } else {
                unlink(subPath.c_str());
            }
        }
    }
    closedir(dir);
}

StorageBridge& StorageBridge::getInstance() {
    static StorageBridge instance;
    return instance;
}

bool StorageBridge::initialize(const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mStorageMutex);
    mSandboxDir = sandboxDir;
    mMediaDir = mSandboxDir + "/data/media/0";

    LOGI("StorageBridge: Initializing Virtual Storage Bridge at %s", mMediaDir.c_str());

    // Create sandbox base dirs
    createDirRecursive(mSandboxDir + "/tmp");
    createDirRecursive(mSandboxDir + "/proc");
    createDirRecursive(mSandboxDir + "/storage/emulated/0");
    createDirRecursive(mSandboxDir + "/data/media/0");

    // Create dummy /dev/fuse backing node
    std::string fusePath = mSandboxDir + "/tmp/dev_fuse.raw";
    int fuseFd = open(fusePath.c_str(), O_RDWR | O_CREAT, 0666);
    if (fuseFd >= 0) {
        close(fuseFd);
    }

    // Populate standard Android directories
    populateStandardFolders();

    // Populate /proc/mounts
    updateMountsFile();

    // Scan metrics & update properties
    StorageStats stats = getStats();
    updatePropertyCache(stats);

    mInitialized = true;
    LOGI("StorageBridge: Successfully initialized virtual storage subsystem.");
    return true;
}

void StorageBridge::shutdown() {
    std::lock_guard<std::mutex> lock(mStorageMutex);
    mInitialized = false;
    LOGI("StorageBridge: Virtual storage bridge shutdown.");
}

bool StorageBridge::populateStandardFolders() {
    const std::vector<std::string> standardDirs = {
        mMediaDir + "/DCIM",
        mMediaDir + "/DCIM/Camera",
        mMediaDir + "/Pictures",
        mMediaDir + "/Pictures/Screenshots",
        mMediaDir + "/Download",
        mMediaDir + "/Music",
        mMediaDir + "/Movies",
        mMediaDir + "/Documents",
        mMediaDir + "/Android",
        mMediaDir + "/Android/data",
        mMediaDir + "/Android/obb",
        mMediaDir + "/Android/media"
    };

    for (const auto& dir : standardDirs) {
        createDirRecursive(dir);
    }

    LOGI("StorageBridge: Standard Android external storage directories verified (%zu folders).", standardDirs.size());
    return true;
}

bool StorageBridge::generateSampleFiles() {
    std::lock_guard<std::mutex> lock(mStorageMutex);
    populateStandardFolders();

    // 1. Download/welcome_gsi.txt
    std::string welcomeFile = mMediaDir + "/Download/welcome_gsi.txt";
    std::ofstream outWelcome(welcomeFile);
    if (outWelcome.is_open()) {
        outWelcome << "========================================================\n"
                   << "  WELCOME TO GSI RUNTIME ENGINE (PILAR 8 - STORAGE VFS) \n"
                   << "========================================================\n"
                   << "Environment: Zero-Root Unprivileged Android Sandbox\n"
                   << "Storage Emulation: FUSE / sdcardfs multi-user simulation\n"
                   << "Path: /storage/emulated/0 (linked to /data/media/0)\n"
                   << "Standard Hierarchy: DCIM, Pictures, Download, Music, Documents\n"
                   << "Generated at runtime successfully.\n";
        outWelcome.close();
    }

    // 2. Documents/system_specs.json
    std::string specsFile = mMediaDir + "/Documents/system_specs.json";
    std::ofstream outSpecs(specsFile);
    if (outSpecs.is_open()) {
        outSpecs << "{\n"
                 << "  \"runtime\": \"GsiRuntime-Engine\",\n"
                 << "  \"version\": \"1.0.0-cyber\",\n"
                 << "  \"storage\": {\n"
                 << "    \"type\": \"fuse_emulated\",\n"
                 << "    \"capacity_gb\": 64,\n"
                 << "    \"mount_point\": \"/storage/emulated/0\"\n"
                 << "  }\n"
                 << "}\n";
        outSpecs.close();
    }

    // 3. DCIM/Camera/IMG_20260905_001.raw
    std::string photoFile = mMediaDir + "/DCIM/Camera/IMG_20260905_001.raw";
    int photoFd = open(photoFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (photoFd >= 0) {
        std::vector<uint8_t> dummyData(32768, 0xAA);
        write(photoFd, dummyData.data(), dummyData.size());
        close(photoFd);
    }

    StorageStats stats = getStats();
    updatePropertyCache(stats);

    LOGI("StorageBridge: Sample files generated successfully (welcome.txt, system_specs.json, IMG_001.raw).");
    return true;
}

bool StorageBridge::wipeStorage() {
    std::lock_guard<std::mutex> lock(mStorageMutex);
    LOGI("StorageBridge: Wiping virtual media storage at %s", mMediaDir.c_str());

    removeDirRecursive(mMediaDir);
    populateStandardFolders();

    StorageStats stats = getStats();
    updatePropertyCache(stats);

    LOGI("StorageBridge: Storage wipe complete. Clean standard directory tree restored.");
    return true;
}

void StorageBridge::updateMountsFile() {
    std::string mountsPath = mSandboxDir + "/proc/mounts";
    std::ofstream out(mountsPath);
    if (out.is_open()) {
        out << "rootfs / rootfs ro,relatime 0 0\n"
            << "tmpfs /dev tmpfs rw,nosuid,relatime,mode=755 0 0\n"
            << "devpts /dev/pts devpts rw,relatime,mode=600 0 0\n"
            << "proc /proc proc rw,relatime 0 0\n"
            << "sysfs /sys sysfs rw,relatime 0 0\n"
            << "/dev/block/vfs_ext4 /system ext4 ro,nodev,relatime 0 0\n"
            << "/dev/block/vfs_ext4 /vendor ext4 ro,nodev,relatime 0 0\n"
            << "/dev/block/vfs_ext4 /data ext4 rw,nosuid,nodev,noatime 0 0\n"
            << "/dev/fuse /storage/emulated fuse rw,nosuid,nodev,noexec,relatime,user_id=0,group_id=0,default_permissions,allow_other 0 0\n"
            << "/data/media /storage/emulated/0 sdcardfs rw,nosuid,nodev,relatime,uid=1023,gid=1023 0 0\n";
        out.close();
        LOGI("StorageBridge: Synthetic /proc/mounts updated successfully at %s", mountsPath.c_str());
    }
}

void StorageBridge::scanStorageMetrics(const std::string& dirPath, uint32_t& dirCount, uint32_t& fileCount, uint64_t& byteSize) {
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return;

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string subPath = dirPath + "/" + entry->d_name;
        struct stat st;
        if (lstat(subPath.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                dirCount++;
                scanStorageMetrics(subPath, dirCount, fileCount, byteSize);
            } else {
                fileCount++;
                byteSize += st.st_size;
            }
        }
    }
    closedir(dir);
}

StorageStats StorageBridge::getStats() {
    StorageStats stats;
    uint32_t dCount = 0;
    uint32_t fCount = 0;
    uint64_t bSize = 0;

    scanStorageMetrics(mMediaDir, dCount, fCount, bSize);

    stats.directoryCount = dCount;
    stats.fileCount = fCount;
    stats.usedBytes = (8ULL * 1024 * 1024 * 1024) + bSize; // ~8GB base system usage + media
    if (stats.usedBytes < stats.totalBytes) {
        stats.freeBytes = stats.totalBytes - stats.usedBytes;
    } else {
        stats.freeBytes = 512ULL * 1024 * 1024;
    }
    stats.isMounted = true;
    return stats;
}

std::string StorageBridge::getStorageStatsString() {
    StorageStats stats = getStats();
    double totalGb = static_cast<double>(stats.totalBytes) / (1024.0 * 1024.0 * 1024.0);
    double freeGb = static_cast<double>(stats.freeBytes) / (1024.0 * 1024.0 * 1024.0);
    double usedGb = static_cast<double>(stats.usedBytes) / (1024.0 * 1024.0 * 1024.0);

    std::ostringstream oss;
    oss << "Storage: /storage/emulated/0 (FUSE/sdcardfs)\n"
        << "Capacity: " << std::fixed << std::setprecision(1) << totalGb << " GB\n"
        << "Used: " << usedGb << " GB (" << stats.directoryCount << " dirs, " << stats.fileCount << " files)\n"
        << "Available: " << freeGb << " GB\n"
        << "State: MOUNTED (RW)";
    return oss.str();
}

void StorageBridge::updatePropertyCache(const StorageStats& stats) {
    auto& propService = PropertyService::getInstance();
    propService.setProperty("persist.sys.storage.type", "fuse");
    propService.setProperty("persist.sys.storage.total_gb", "64");
    
    double freeGb = static_cast<double>(stats.freeBytes) / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream freeOss;
    freeOss << std::fixed << std::setprecision(1) << freeGb;
    propService.setProperty("persist.sys.storage.avail_gb", freeOss.str());
    propService.setProperty("persist.sys.storage.state", "mounted");
    propService.setProperty("persist.sys.storage.dirs", std::to_string(stats.directoryCount));
    propService.setProperty("persist.sys.storage.files", std::to_string(stats.fileCount));
}

} // namespace gsi
