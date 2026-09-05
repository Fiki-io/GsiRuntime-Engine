#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <sys/types.h>

namespace gsi {

class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;
    virtual bool readBlock(uint64_t blockNum, void* buffer) = 0;
    virtual bool readBlocks(uint64_t startBlock, uint32_t count, void* buffer) = 0;
    virtual uint32_t getBlockSize() const = 0;
    virtual uint64_t getTotalBlocks() const = 0;
};

class RawBlockDevice : public IBlockDevice {
public:
    RawBlockDevice(int fd, uint32_t blockSize, uint64_t totalBlocks);
    ~RawBlockDevice() override = default;

    bool readBlock(uint64_t blockNum, void* buffer) override;
    bool readBlocks(uint64_t startBlock, uint32_t count, void* buffer) override;
    uint32_t getBlockSize() const override { return mBlockSize; }
    uint64_t getTotalBlocks() const override { return mTotalBlocks; }

private:
    int mFd;
    uint32_t mBlockSize;
    uint64_t mTotalBlocks;
};

struct SparseChunk {
    uint64_t virtualBlockStart;
    uint32_t blockCount;
    uint16_t chunkType;
    uint32_t fillVal;
    off_t fileOffset; // offset in sparse file
};

class SparseBlockDevice : public IBlockDevice {
public:
    static std::unique_ptr<SparseBlockDevice> create(int fd);
    ~SparseBlockDevice() override = default;

    bool readBlock(uint64_t blockNum, void* buffer) override;
    bool readBlocks(uint64_t startBlock, uint32_t count, void* buffer) override;
    uint32_t getBlockSize() const override { return mBlockSize; }
    uint64_t getTotalBlocks() const override { return mTotalBlocks; }

private:
    SparseBlockDevice(int fd, uint32_t blockSize, uint64_t totalBlocks);
    bool buildIndex();

    int mFd;
    uint32_t mBlockSize;
    uint64_t mTotalBlocks;
    std::vector<SparseChunk> mChunks;
};

} // namespace gsi
