#include "include/image_verifier.h"
#include "include/logger.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace gsi {

constexpr uint32_t ANDROID_SPARSE_MAGIC = 0xED26FF3A;
constexpr uint16_t EXT4_SUPERBLOCK_MAGIC = 0xEF53;
constexpr off_t EXT4_SUPERBLOCK_OFFSET = 1024;

#pragma pack(push, 1)
struct SparseHeader {
    uint32_t magic;          // 0xED26FF3A
    uint16_t major_version;  // (0x1)
    uint16_t minor_version;  // (0x0)
    uint16_t file_hdr_sz;    // 28 bytes
    uint16_t chunk_hdr_sz;   // 12 bytes
    uint32_t blk_sz;         // Block size in bytes (e.g. 4096)
    uint32_t total_blks;     // Total blocks in uncompressed output
    uint32_t total_chunks;   // Total chunks in sparse image
    uint32_t image_checksum; // CRC32 checksum or 0
};

struct Ext4SuperblockHeader {
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
    uint16_t s_magic;       // 0xEF53 at offset 0x38 (56 bytes from superblock start)
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // EXT4_DYNAMIC_REV specific fields:
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
};
#pragma pack(pop)

ImageVerificationResult ImageVerifier::verifyFileDescriptor(int fd) {
    ImageVerificationResult result;
    if (fd < 0) {
        result.description = "Invalid file descriptor";
        return result;
    }

    // 1. Check for Android Sparse Header at offset 0
    SparseHeader sparseHdr{};
    ssize_t bytesRead = pread(fd, &sparseHdr, sizeof(sparseHdr), 0);
    if (bytesRead == sizeof(sparseHdr) && sparseHdr.magic == ANDROID_SPARSE_MAGIC) {
        result.isValid = true;
        result.format = ImageFormat::ANDROID_SPARSE;
        result.blockSize = sparseHdr.blk_sz;
        result.totalBlocks = sparseHdr.total_blks;
        result.uncompressedSizeBytes = static_cast<uint64_t>(sparseHdr.blk_sz) * sparseHdr.total_blks;

        std::ostringstream oss;
        oss << "Android Sparse Image (v" << sparseHdr.major_version << "." << sparseHdr.minor_version
            << ") | Block Size: " << sparseHdr.blk_sz << " B"
            << " | Total Blocks: " << sparseHdr.total_blks
            << " | Uncompressed Size: " << (result.uncompressedSizeBytes / (1024 * 1024)) << " MB";
        result.description = oss.str();
        LOGI("Image verification SUCCESS: %s", result.description.c_str());
        return result;
    }

    // 2. Check for EXT4 Superblock at offset 1024
    Ext4SuperblockHeader ext4Hdr{};
    bytesRead = pread(fd, &ext4Hdr, sizeof(ext4Hdr), EXT4_SUPERBLOCK_OFFSET);
    if (bytesRead == sizeof(ext4Hdr) && ext4Hdr.s_magic == EXT4_SUPERBLOCK_MAGIC) {
        result.isValid = true;
        result.format = ImageFormat::RAW_EXT4;
        result.blockSize = 1024u << ext4Hdr.s_log_block_size;
        result.totalBlocks = ext4Hdr.s_blocks_count_lo;
        result.uncompressedSizeBytes = static_cast<uint64_t>(result.blockSize) * result.totalBlocks;

        char volName[17] = {0};
        std::memcpy(volName, ext4Hdr.s_volume_name, 16);
        result.volumeName = volName;

        std::ostringstream oss;
        oss << "Raw EXT4 Partition Image | Volume: \"" << result.volumeName << "\""
            << " | Block Size: " << result.blockSize << " B"
            << " | Total Blocks: " << result.totalBlocks
            << " | Size: " << (result.uncompressedSizeBytes / (1024 * 1024)) << " MB";
        result.description = oss.str();
        LOGI("Image verification SUCCESS: %s", result.description.c_str());
        return result;
    }

    result.isValid = false;
    result.format = ImageFormat::UNKNOWN;
    result.description = "Unknown or unsupported image format. Not a valid Android Sparse or EXT4 image.";
    LOGW("Image verification FAILED: Unknown format");
    return result;
}

ImageVerificationResult ImageVerifier::verifyFilePath(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        ImageVerificationResult res;
        res.description = "Failed to open file path: " + path;
        return res;
    }
    ImageVerificationResult res = verifyFileDescriptor(fd);
    close(fd);
    return res;
}

} // namespace gsi
