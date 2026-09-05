#include "include/ext4_reader.h"
#include "include/logger.h"

#include <cstring>
#include <sstream>
#include <algorithm>

namespace gsi {

constexpr uint16_t EXT4_MAGIC = 0xEF53;
constexpr uint16_t EXT4_EXTENTS_MAGIC = 0xF30A;
constexpr uint32_t EXT4_ROOT_INO = 2;
constexpr uint32_t INCOMPAT_64BIT = 0x0080;

#pragma pack(push, 1)
struct RawSuperblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
};

struct RawGroupDesc32 {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
};

struct RawGroupDesc64 {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
};

struct RawInode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint8_t  i_block[60];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
};

struct RawExtentHeader {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
};

struct RawExtent {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
};

struct RawExtentIdx {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
};

struct RawDirEntry2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
};
#pragma pack(pop)

Ext4Reader::Ext4Reader(std::shared_ptr<IBlockDevice> dev) : mDev(std::move(dev)) {}

std::unique_ptr<Ext4Reader> Ext4Reader::open(std::shared_ptr<IBlockDevice> dev) {
    if (!dev) return nullptr;
    auto reader = std::unique_ptr<Ext4Reader>(new Ext4Reader(dev));
    if (!reader->init()) {
        return nullptr;
    }
    return reader;
}

bool Ext4Reader::init() {
    std::vector<uint8_t> blockBuf(mDev->getBlockSize());
    // Superblock is at byte offset 1024.
    // If block size is 1024, it is at block 1. If block size is >= 2048, it is in block 0 at offset 1024.
    uint64_t sbBlock = (mDev->getBlockSize() == 1024) ? 1 : 0;
    size_t sbOffset = (mDev->getBlockSize() == 1024) ? 0 : 1024;

    if (!mDev->readBlock(sbBlock, blockBuf.data())) {
        LOGE("Ext4Reader: Failed to read superblock block %llu", (unsigned long long)sbBlock);
        return false;
    }

    RawSuperblock sb{};
    std::memcpy(&sb, blockBuf.data() + sbOffset, sizeof(sb));

    if (sb.s_magic != EXT4_MAGIC) {
        LOGE("Ext4Reader: Magic mismatch 0x%X (expected 0xEF53)", sb.s_magic);
        return false;
    }

    mBlockSize = 1024u << sb.s_log_block_size;
    mInodesPerGroup = sb.s_inodes_per_group;
    mInodeSize = sb.s_inode_size ? sb.s_inode_size : 128;
    mIs64Bit = (sb.s_feature_incompat & INCOMPAT_64BIT) != 0;
    mDescSize = mIs64Bit ? (sb.s_desc_size ? sb.s_desc_size : 64) : 32;

    char vol[17] = {0};
    std::memcpy(vol, sb.s_volume_name, 16);
    mVolumeName = vol;

    LOGI("Ext4Reader: Initialized EXT4 filesystem. BlockSize: %u, InodeSize: %u, 64Bit: %d, Vol: \"%s\"",
         mBlockSize, mInodeSize, mIs64Bit, mVolumeName.c_str());
    return true;
}

uint64_t Ext4Reader::getInodeTableBlock(uint32_t group) {
    uint64_t gdtStartBlock = (mBlockSize == 1024) ? 2 : 1;
    uint64_t descOffsetBytes = static_cast<uint64_t>(group) * mDescSize;
    uint64_t gdtBlock = gdtStartBlock + (descOffsetBytes / mBlockSize);
    size_t offsetInBlock = descOffsetBytes % mBlockSize;

    std::vector<uint8_t> blockBuf(mBlockSize);
    if (!mDev->readBlock(gdtBlock, blockBuf.data())) {
        return 0;
    }

    if (mIs64Bit) {
        RawGroupDesc64 gd{};
        std::memcpy(&gd, blockBuf.data() + offsetInBlock, sizeof(gd));
        return (static_cast<uint64_t>(gd.bg_inode_table_hi) << 32) | gd.bg_inode_table_lo;
    } else {
        RawGroupDesc32 gd{};
        std::memcpy(&gd, blockBuf.data() + offsetInBlock, sizeof(gd));
        return gd.bg_inode_table_lo;
    }
}

bool Ext4Reader::readInode(uint32_t ino, void* outInodeBuf) {
    if (ino == 0 || mInodesPerGroup == 0) return false;

    uint32_t group = (ino - 1) / mInodesPerGroup;
    uint32_t index = (ino - 1) % mInodesPerGroup;

    uint64_t itableBlock = getInodeTableBlock(group);
    if (itableBlock == 0) return false;

    uint64_t byteOffset = static_cast<uint64_t>(index) * mInodeSize;
    uint64_t targetBlock = itableBlock + (byteOffset / mBlockSize);
    size_t offsetInBlock = byteOffset % mBlockSize;

    std::vector<uint8_t> blockBuf(mBlockSize);
    if (!mDev->readBlock(targetBlock, blockBuf.data())) {
        return false;
    }

    std::memcpy(outInodeBuf, blockBuf.data() + offsetInBlock, sizeof(RawInode));
    return true;
}

void Ext4Reader::readExtentTree(const uint8_t* extentHeaderPtr, std::vector<std::pair<uint64_t, uint32_t>>& blockRanges) {
    RawExtentHeader hdr{};
    std::memcpy(&hdr, extentHeaderPtr, sizeof(hdr));

    if (hdr.eh_magic != EXT4_EXTENTS_MAGIC) {
        return;
    }

    if (hdr.eh_depth == 0) {
        // Leaf nodes
        const RawExtent* extents = reinterpret_cast<const RawExtent*>(extentHeaderPtr + sizeof(RawExtentHeader));
        for (uint16_t i = 0; i < hdr.eh_entries; ++i) {
            uint64_t physBlock = (static_cast<uint64_t>(extents[i].ee_start_hi) << 32) | extents[i].ee_start_lo;
            uint32_t count = extents[i].ee_len;
            if (count > 32768) count -= 32768; // uninitialized extent flag
            blockRanges.emplace_back(physBlock, count);
        }
    } else {
        // Index nodes
        const RawExtentIdx* idxs = reinterpret_cast<const RawExtentIdx*>(extentHeaderPtr + sizeof(RawExtentHeader));
        std::vector<uint8_t> childBuf(mBlockSize);
        for (uint16_t i = 0; i < hdr.eh_entries; ++i) {
            uint64_t childBlock = (static_cast<uint64_t>(idxs[i].ei_leaf_hi) << 32) | idxs[i].ei_leaf_lo;
            if (mDev->readBlock(childBlock, childBuf.data())) {
                readExtentTree(childBuf.data(), blockRanges);
            }
        }
    }
}

bool Ext4Reader::readFileDataByInode(uint32_t ino, std::vector<uint8_t>& outData) {
    RawInode inode{};
    if (!readInode(ino, &inode)) return false;

    uint64_t fileSize = (static_cast<uint64_t>(inode.i_size_high) << 32) | inode.i_size_lo;
    outData.clear();
    outData.reserve(fileSize);

    std::vector<std::pair<uint64_t, uint32_t>> blockRanges;
    readExtentTree(inode.i_block, blockRanges);

    uint64_t bytesRemaining = fileSize;
    std::vector<uint8_t> blockBuf(mBlockSize);

    for (const auto& range : blockRanges) {
        uint64_t startBlock = range.first;
        uint32_t count = range.second;

        for (uint32_t b = 0; b < count && bytesRemaining > 0; ++b) {
            if (!mDev->readBlock(startBlock + b, blockBuf.data())) {
                return false;
            }
            size_t toCopy = std::min(bytesRemaining, static_cast<uint64_t>(mBlockSize));
            outData.insert(outData.end(), blockBuf.begin(), blockBuf.begin() + toCopy);
            bytesRemaining -= toCopy;
        }
    }

    return true;
}

uint32_t Ext4Reader::findChildInode(uint32_t dirIno, const std::string& name) {
    std::vector<uint8_t> dirData;
    if (!readFileDataByInode(dirIno, dirData)) return 0;

    size_t offset = 0;
    while (offset + sizeof(RawDirEntry2) <= dirData.size()) {
        const RawDirEntry2* de = reinterpret_cast<const RawDirEntry2*>(dirData.data() + offset);
        if (de->rec_len == 0) break;

        if (de->inode != 0 && de->name_len == name.length()) {
            const char* entryName = reinterpret_cast<const char*>(dirData.data() + offset + sizeof(RawDirEntry2));
            if (std::memcmp(entryName, name.data(), name.length()) == 0) {
                return de->inode;
            }
        }
        offset += de->rec_len;
    }
    return 0;
}

uint32_t Ext4Reader::resolvePath(const std::string& path) {
    if (path.empty() || path == "/") return EXT4_ROOT_INO;

    std::stringstream ss(path);
    std::string item;
    uint32_t currentIno = EXT4_ROOT_INO;

    while (std::getline(ss, item, '/')) {
        if (item.empty() || item == ".") continue;
        currentIno = findChildInode(currentIno, item);
        if (currentIno == 0) return 0;
    }
    return currentIno;
}

bool Ext4Reader::listDirectory(const std::string& path, std::vector<FsFileEntry>& outEntries) {
    outEntries.clear();
    uint32_t dirIno = resolvePath(path);
    if (dirIno == 0) return false;

    std::vector<uint8_t> dirData;
    if (!readFileDataByInode(dirIno, dirData)) return false;

    size_t offset = 0;
    while (offset + sizeof(RawDirEntry2) <= dirData.size()) {
        const RawDirEntry2* de = reinterpret_cast<const RawDirEntry2*>(dirData.data() + offset);
        if (de->rec_len == 0) break;

        if (de->inode != 0) {
            std::string entryName(
                reinterpret_cast<const char*>(dirData.data() + offset + sizeof(RawDirEntry2)),
                de->name_len
            );

            if (entryName != "." && entryName != "..") {
                FsFileEntry entry{};
                entry.name = entryName;
                entry.path = (path == "/" ? "" : path) + "/" + entryName;
                entry.inode = de->inode;
                entry.isDirectory = (de->file_type == 2);
                entry.isSymlink = (de->file_type == 7);

                RawInode childInode{};
                if (readInode(de->inode, &childInode)) {
                    entry.size = (static_cast<uint64_t>(childInode.i_size_high) << 32) | childInode.i_size_lo;
                }

                outEntries.push_back(entry);
            }
        }
        offset += de->rec_len;
    }

    // Sort folders first, then alphabetical
    std::sort(outEntries.begin(), outEntries.end(), [](const FsFileEntry& a, const FsFileEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });

    return true;
}

bool Ext4Reader::readFile(const std::string& path, std::vector<uint8_t>& outData) {
    uint32_t ino = resolvePath(path);
    if (ino == 0) return false;
    return readFileDataByInode(ino, outData);
}

bool Ext4Reader::readFileString(const std::string& path, std::string& outStr) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) return false;
    outStr.assign(bytes.begin(), bytes.end());
    return true;
}

BuildPropInfo Ext4Reader::extractBuildProp() {
    BuildPropInfo info;

    // Standard Android paths for build.prop
    std::vector<std::string> candidatePaths = {
        "/system/build.prop",
        "/build.prop",
        "/system/system/build.prop"
    };

    std::string propText;
    for (const auto& p : candidatePaths) {
        if (readFileString(p, propText) && !propText.empty()) {
            LOGI("Found build.prop at %s (%zu bytes)", p.c_str(), propText.size());
            break;
        }
    }

    if (propText.empty()) {
        LOGW("build.prop not found in standard paths");
        return info;
    }

    info.rawContent = propText;
    std::istringstream stream(propText);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string key = line.substr(0, eqPos);
            std::string val = line.substr(eqPos + 1);

            // Trim carriage returns
            if (!val.empty() && val.back() == '\r') val.pop_back();

            if (key == "ro.build.version.release") info.osVersion = val;
            else if (key == "ro.build.version.sdk") info.sdkVersion = val;
            else if (key == "ro.build.id") info.buildId = val;
            else if (key == "ro.build.version.security_patch") info.securityPatch = val;
            else if (key == "ro.product.model" || key == "ro.product.system.model") {
                if (info.model.empty()) info.model = val;
            }
            else if (key == "ro.system.build.fingerprint" || key == "ro.build.fingerprint") {
                if (info.fingerprint.empty()) info.fingerprint = val;
            }
            else if (key == "ro.treble.enabled") {
                info.isTrebleEnabled = (val == "true");
            }
        }
    }

    return info;
}

} // namespace gsi
