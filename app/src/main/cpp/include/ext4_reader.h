#pragma once

#include "block_device.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace gsi {

struct FsFileEntry {
    std::string name;
    std::string path;
    uint32_t inode = 0;
    uint64_t size = 0;
    bool isDirectory = false;
    bool isSymlink = false;
};

struct BuildPropInfo {
    std::string osVersion;       // ro.build.version.release (e.g. "14")
    std::string sdkVersion;      // ro.build.version.sdk (e.g. "34")
    std::string buildId;         // ro.build.id
    std::string securityPatch;   // ro.build.version.security_patch
    std::string model;           // ro.product.model
    std::string fingerprint;     // ro.system.build.fingerprint
    bool isTrebleEnabled = true;
    std::string rawContent;
};

class Ext4Reader {
public:
    static std::unique_ptr<Ext4Reader> open(std::shared_ptr<IBlockDevice> dev);
    ~Ext4Reader() = default;

    bool listDirectory(const std::string& path, std::vector<FsFileEntry>& outEntries);
    bool readFile(const std::string& path, std::vector<uint8_t>& outData);
    bool readFileString(const std::string& path, std::string& outStr);

    BuildPropInfo extractBuildProp();

    uint32_t getBlockSize() const { return mBlockSize; }
    std::string getVolumeName() const { return mVolumeName; }

private:
    explicit Ext4Reader(std::shared_ptr<IBlockDevice> dev);
    bool init();

    uint64_t getInodeTableBlock(uint32_t group);
    bool readInode(uint32_t ino, void* outInodeBuf);
    bool readFileDataByInode(uint32_t ino, std::vector<uint8_t>& outData);
    void readExtentTree(const uint8_t* extentHeaderPtr, std::vector<std::pair<uint64_t, uint32_t>>& blockRanges);
    uint32_t findChildInode(uint32_t dirIno, const std::string& name);
    uint32_t resolvePath(const std::string& path);

    std::shared_ptr<IBlockDevice> mDev;
    uint32_t mBlockSize = 4096;
    uint32_t mInodesPerGroup = 0;
    uint16_t mInodeSize = 256;
    uint16_t mDescSize = 32;
    bool mIs64Bit = false;
    std::string mVolumeName;
};

} // namespace gsi
