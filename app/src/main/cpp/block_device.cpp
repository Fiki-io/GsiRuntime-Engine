#include "include/block_device.h"
#include "include/logger.h"

#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace gsi {

constexpr uint16_t CHUNK_TYPE_RAW = 0xCAC1;
constexpr uint16_t CHUNK_TYPE_FILL = 0xCAC2;
constexpr uint16_t CHUNK_TYPE_DONT_CARE = 0xCAC3;

#pragma pack(push, 1)
struct RawSparseHeader {
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

struct RawChunkHeader {
    uint16_t chunk_type;
    uint16_t reserved1;
    uint32_t chunk_sz;       // chunk size in blocks
    uint32_t total_sz;       // total size in bytes of chunk header + data
};
#pragma pack(pop)

// -------------------------------------------------------------
// RawBlockDevice Implementation
// -------------------------------------------------------------

RawBlockDevice::RawBlockDevice(int fd, uint32_t blockSize, uint64_t totalBlocks)
    : mFd(fd), mBlockSize(blockSize), mTotalBlocks(totalBlocks) {}

bool RawBlockDevice::readBlock(uint64_t blockNum, void* buffer) {
    if (blockNum >= mTotalBlocks) {
        LOGE("RawBlockDevice: Read out of bounds. Block: %llu, Total: %llu",
             (unsigned long long)blockNum, (unsigned long long)mTotalBlocks);
        return false;
    }
    off_t offset = static_cast<off_t>(blockNum * mBlockSize);
    ssize_t bytes = pread(mFd, buffer, mBlockSize, offset);
    return (bytes == static_cast<ssize_t>(mBlockSize));
}

bool RawBlockDevice::readBlocks(uint64_t startBlock, uint32_t count, void* buffer) {
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    for (uint32_t i = 0; i < count; ++i) {
        if (!readBlock(startBlock + i, ptr + (i * mBlockSize))) {
            return false;
        }
    }
    return true;
}

// -------------------------------------------------------------
// SparseBlockDevice Implementation
// -------------------------------------------------------------

SparseBlockDevice::SparseBlockDevice(int fd, uint32_t blockSize, uint64_t totalBlocks)
    : mFd(fd), mBlockSize(blockSize), mTotalBlocks(totalBlocks) {}

std::unique_ptr<SparseBlockDevice> SparseBlockDevice::create(int fd) {
    RawSparseHeader hdr{};
    if (pread(fd, &hdr, sizeof(hdr), 0) != sizeof(hdr) || hdr.magic != 0xED26FF3A) {
        LOGE("SparseBlockDevice: File descriptor is not a valid Android sparse image");
        return nullptr;
    }

    auto dev = std::unique_ptr<SparseBlockDevice>(
        new SparseBlockDevice(fd, hdr.blk_sz, hdr.total_blks)
    );

    if (!dev->buildIndex()) {
        LOGE("SparseBlockDevice: Failed to index sparse chunks");
        return nullptr;
    }

    return dev;
}

bool SparseBlockDevice::buildIndex() {
    RawSparseHeader hdr{};
    if (pread(mFd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) return false;

    off_t fileOffset = hdr.file_hdr_sz;
    uint64_t currentVirtualBlock = 0;
    mChunks.reserve(hdr.total_chunks);

    for (uint32_t i = 0; i < hdr.total_chunks; ++i) {
        RawChunkHeader chunkHdr{};
        if (pread(mFd, &chunkHdr, sizeof(chunkHdr), fileOffset) != sizeof(chunkHdr)) {
            LOGE("SparseBlockDevice: Truncated chunk header at offset %lld", (long long)fileOffset);
            return false;
        }

        SparseChunk chunk{};
        chunk.virtualBlockStart = currentVirtualBlock;
        chunk.blockCount = chunkHdr.chunk_sz;
        chunk.chunkType = chunkHdr.chunk_type;
        chunk.fileOffset = fileOffset + hdr.chunk_hdr_sz;
        chunk.fillVal = 0;

        if (chunkHdr.chunk_type == CHUNK_TYPE_FILL) {
            uint32_t fill = 0;
            pread(mFd, &fill, sizeof(fill), chunk.fileOffset);
            chunk.fillVal = fill;
        }

        mChunks.push_back(chunk);
        currentVirtualBlock += chunkHdr.chunk_sz;
        fileOffset += chunkHdr.total_sz;
    }

    LOGI("SparseBlockDevice: Successfully indexed %zu chunks (Total Virtual Blocks: %llu)",
         mChunks.size(), (unsigned long long)mTotalBlocks);
    return true;
}

bool SparseBlockDevice::readBlock(uint64_t blockNum, void* buffer) {
    if (blockNum >= mTotalBlocks || mChunks.empty()) {
        return false;
    }

    // Binary search to find the chunk containing blockNum
    auto it = std::upper_bound(
        mChunks.begin(), mChunks.end(), blockNum,
        [](uint64_t b, const SparseChunk& c) {
            return b < c.virtualBlockStart;
        }
    );

    if (it == mChunks.begin()) return false;
    --it;

    const SparseChunk& chunk = *it;
    if (blockNum < chunk.virtualBlockStart || blockNum >= (chunk.virtualBlockStart + chunk.blockCount)) {
        return false;
    }

    uint64_t blockOffsetInChunk = blockNum - chunk.virtualBlockStart;

    switch (chunk.chunkType) {
        case CHUNK_TYPE_RAW: {
            off_t offsetInFile = chunk.fileOffset + static_cast<off_t>(blockOffsetInChunk * mBlockSize);
            ssize_t bytes = pread(mFd, buffer, mBlockSize, offsetInFile);
            return (bytes == static_cast<ssize_t>(mBlockSize));
        }
        case CHUNK_TYPE_FILL: {
            uint32_t* p = static_cast<uint32_t*>(buffer);
            uint32_t count = mBlockSize / sizeof(uint32_t);
            for (uint32_t i = 0; i < count; ++i) {
                p[i] = chunk.fillVal;
            }
            return true;
        }
        case CHUNK_TYPE_DONT_CARE:
        default:
            std::memset(buffer, 0, mBlockSize);
            return true;
    }
}

bool SparseBlockDevice::readBlocks(uint64_t startBlock, uint32_t count, void* buffer) {
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    for (uint32_t i = 0; i < count; ++i) {
        if (!readBlock(startBlock + i, ptr + (i * mBlockSize))) {
            return false;
        }
    }
    return true;
}

} // namespace gsi
