#include "include/sandbox_manager.h"
#include "include/logger.h"

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

namespace gsi {

SandboxManager& SandboxManager::getInstance() {
    static SandboxManager instance;
    return instance;
}

static bool createDirIfNotExists(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return (mkdir(path.c_str(), 0755) == 0);
}

bool SandboxManager::initializeSandbox(const std::string& basePath) {
    mBasePath = basePath;
    mBinDir = basePath + "/bin";
    mLibDir = basePath + "/lib64";
    mDataDir = basePath + "/data";
    mTmpDir = basePath + "/tmp";

    if (!createDirIfNotExists(mBasePath)) return false;
    if (!createDirIfNotExists(mBinDir)) return false;
    if (!createDirIfNotExists(mLibDir)) return false;
    if (!createDirIfNotExists(mDataDir)) return false;
    if (!createDirIfNotExists(mTmpDir)) return false;

    LOGI("Sandbox directories initialized at: %s", mBasePath.c_str());
    return true;
}

bool SandboxManager::deployHookLibrary(const std::string& hostLibDir) {
    std::string srcPath = hostLibDir + "/libgsi_hook.so";
    std::string dstPath = mLibDir + "/libgsi_hook.so";

    int srcFd = open(srcPath.c_str(), O_RDONLY);
    if (srcFd < 0) {
        LOGW("deployHookLibrary: Host hook library not found at: %s", srcPath.c_str());
        return false;
    }

    int dstFd = open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dstFd < 0) {
        close(srcFd);
        LOGE("deployHookLibrary: Failed to create destination at: %s", dstPath.c_str());
        return false;
    }

    char buffer[4096];
    ssize_t bytes;
    while ((bytes = read(srcFd, buffer, sizeof(buffer))) > 0) {
        write(dstFd, buffer, bytes);
    }

    close(srcFd);
    close(dstFd);
    chmod(dstPath.c_str(), 0755);

    LOGI("deployHookLibrary: Successfully deployed libgsi_hook.so to %s (0755)", dstPath.c_str());
    return true;
}

bool SandboxManager::extractBinaryFromVfs(Ext4Reader& reader, const std::string& vfsPath, const std::string& targetName) {
    std::vector<uint8_t> fileData;
    if (!reader.readFile(vfsPath, fileData) || fileData.empty()) {
        LOGE("Failed to read binary from VFS: %s", vfsPath.c_str());
        return false;
    }

    std::string destPath = mBinDir + "/" + targetName;
    int fd = open(destPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        LOGE("Failed to open destination path for writing: %s", destPath.c_str());
        return false;
    }

    ssize_t written = write(fd, fileData.data(), fileData.size());
    close(fd);

    if (written != static_cast<ssize_t>(fileData.size())) {
        LOGE("Short write when extracting binary: %s", destPath.c_str());
        return false;
    }

    // Ensure executable permissions
    chmod(destPath.c_str(), 0755);

    LOGI("Extracted %s -> %s (%zu bytes, 0755)", vfsPath.c_str(), destPath.c_str(), fileData.size());
    return true;
}

} // namespace gsi
