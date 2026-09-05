#pragma once

#include "ext4_reader.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace gsi {

struct ExtractionResult {
    bool success = false;
    int totalFiles = 0;
    int totalDirs = 0;
    uint64_t totalBytes = 0;
    std::string summary;
};

using ExtractionProgressCallback = std::function<void(int current, const std::string& currentPath)>;

class GsiExtractor {
public:
    static GsiExtractor& getInstance();

    ExtractionResult extractEssentialSystem(Ext4Reader& reader,
                                           const std::string& sandboxDir,
                                           ExtractionProgressCallback callback = nullptr);

    ExtractionResult extractDirectoryRecursive(Ext4Reader& reader,
                                              const std::string& vfsDirPath,
                                              const std::string& hostTargetDir,
                                              int maxDepth,
                                              ExtractionProgressCallback callback = nullptr);

private:
    GsiExtractor() = default;
    ~GsiExtractor() = default;

    bool ensureDir(const std::string& path, mode_t mode = 0775);
    bool extractSingleFile(Ext4Reader& reader,
                           const std::string& vfsPath,
                           const std::string& hostPath,
                           mode_t mode);
};

} // namespace gsi
