#include "include/gsi_extractor.h"
#include "include/logger.h"

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sstream>

#undef LOG_TAG
#define LOG_TAG "GSI_Extractor"

namespace gsi {

GsiExtractor& GsiExtractor::getInstance() {
    static GsiExtractor instance;
    return instance;
}

bool GsiExtractor::ensureDir(const std::string& path, mode_t mode) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            mkdir(current.c_str(), mode);
        }
    }
    return true;
}

bool GsiExtractor::extractSingleFile(Ext4Reader& reader,
                                    const std::string& vfsPath,
                                    const std::string& hostPath,
                                    mode_t mode) {
    std::vector<uint8_t> data;
    if (!reader.readFile(vfsPath, data) || data.empty()) {
        return false;
    }

    int fd = open(hostPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        LOGE("GsiExtractor: Failed to create host file %s", hostPath.c_str());
        return false;
    }

    ssize_t written = write(fd, data.data(), data.size());
    close(fd);

    if (written == static_cast<ssize_t>(data.size())) {
        chmod(hostPath.c_str(), mode);
        return true;
    }
    return false;
}

ExtractionResult GsiExtractor::extractDirectoryRecursive(Ext4Reader& reader,
                                                        const std::string& vfsDirPath,
                                                        const std::string& hostTargetDir,
                                                        int maxDepth,
                                                        ExtractionProgressCallback callback) {
    ExtractionResult result;
    if (maxDepth < 0) return result;

    ensureDir(hostTargetDir);

    std::vector<FsFileEntry> entries;
    if (!reader.listDirectory(vfsDirPath, entries)) {
        LOGW("GsiExtractor: Directory not found or empty in VFS: %s", vfsDirPath.c_str());
        return result;
    }

    bool isExecutableDir = (vfsDirPath.find("/bin") != std::string::npos ||
                            vfsDirPath.find("/xbin") != std::string::npos);

    for (const auto& entry : entries) {
        if (entry.name == "." || entry.name == "..") continue;

        std::string subVfs = (vfsDirPath == "/") ? ("/" + entry.name) : (vfsDirPath + "/" + entry.name);
        std::string subHost = hostTargetDir + "/" + entry.name;

        if (entry.isDirectory) {
            result.totalDirs++;
            auto subRes = extractDirectoryRecursive(reader, subVfs, subHost, maxDepth - 1, callback);
            result.totalFiles += subRes.totalFiles;
            result.totalDirs += subRes.totalDirs;
            result.totalBytes += subRes.totalBytes;
        } else {
            mode_t mode = isExecutableDir ? 0755 : 0644;
            if (extractSingleFile(reader, subVfs, subHost, mode)) {
                result.totalFiles++;
                result.totalBytes += entry.size;
                if (callback) {
                    callback(result.totalFiles, subVfs);
                }
            }
        }
    }

    result.success = (result.totalFiles > 0);
    return result;
}

ExtractionResult GsiExtractor::extractEssentialSystem(Ext4Reader& reader,
                                                     const std::string& sandboxDir,
                                                     ExtractionProgressCallback callback) {
    ExtractionResult overall;
    LOGI("GsiExtractor: Beginning extraction of essential system from raw GSI VFS...");

    // 1. Prepare base sandbox directories
    ensureDir(sandboxDir + "/system/bin");
    ensureDir(sandboxDir + "/system/etc/init");
    ensureDir(sandboxDir + "/system/lib64");
    ensureDir(sandboxDir + "/bin");
    ensureDir(sandboxDir + "/etc");
    ensureDir(sandboxDir + "/lib64");

    // 2. Extract /system/bin (Native Executables: toybox, sh, linker64, toolbox, etc.)
    LOGI("GsiExtractor: Extracting /system/bin...");
    auto binRes = extractDirectoryRecursive(reader, "/system/bin", sandboxDir + "/system/bin", 1, callback);
    if (!binRes.success) {
        // Fallback for non-system-as-root rootfs
        binRes = extractDirectoryRecursive(reader, "/bin", sandboxDir + "/system/bin", 1, callback);
    }
    overall.totalFiles += binRes.totalFiles;
    overall.totalDirs += binRes.totalDirs;
    overall.totalBytes += binRes.totalBytes;

    // Mirror vital binaries to $SANDBOX/bin for immediate shell lookup
    const std::vector<std::string> vitalBinaries = {"toybox", "sh", "toolbox", "linker64", "linker"};
    for (const auto& bin : vitalBinaries) {
        std::string src = sandboxDir + "/system/bin/" + bin;
        std::string dst = sandboxDir + "/bin/" + bin;
        if (access(src.c_str(), F_OK) == 0 && access(dst.c_str(), F_OK) != 0) {
            extractSingleFile(reader, "/system/bin/" + bin, dst, 0755);
        }
    }

    // 3. Extract /system/etc/init (init.rc configs and service definitions)
    LOGI("GsiExtractor: Extracting /system/etc/init...");
    auto initRes = extractDirectoryRecursive(reader, "/system/etc/init", sandboxDir + "/system/etc/init", 1, callback);
    overall.totalFiles += initRes.totalFiles;
    overall.totalDirs += initRes.totalDirs;
    overall.totalBytes += initRes.totalBytes;

    // 4. Extract /system/build.prop and /init.rc
    std::string buildPropHost = sandboxDir + "/system/build.prop";
    if (extractSingleFile(reader, "/system/build.prop", buildPropHost, 0644)) {
        overall.totalFiles++;
    }
    std::string initRcHost = sandboxDir + "/system/etc/init.rc";
    if (extractSingleFile(reader, "/system/etc/init.rc", initRcHost, 0644)) {
        overall.totalFiles++;
    }

    // 5. Extract critical libraries from /system/lib64 (libc.so, libm.so, libdl.so, etc.)
    LOGI("GsiExtractor: Extracting /system/lib64 runtime libraries...");
    auto libRes = extractDirectoryRecursive(reader, "/system/lib64", sandboxDir + "/system/lib64", 1, callback);
    overall.totalFiles += libRes.totalFiles;
    overall.totalDirs += libRes.totalDirs;
    overall.totalBytes += libRes.totalBytes;

    overall.success = (overall.totalFiles > 0);

    std::ostringstream oss;
    double mbSize = static_cast<double>(overall.totalBytes) / (1024.0 * 1024.0);
    oss << "Extracted " << overall.totalFiles << " essential GSI files ("
        << overall.totalDirs << " directories, "
        << mbSize << " MB) to " << sandboxDir << "/system";
    overall.summary = oss.str();

    LOGI("GsiExtractor: Extraction finished. %s", overall.summary.c_str());
    return overall;
}

} // namespace gsi
