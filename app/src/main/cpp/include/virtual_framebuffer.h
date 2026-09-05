#pragma once

#include <cstdint>
#include <string>
#include <mutex>

namespace gsi {

#pragma pack(push, 1)
struct VfbHeader {
    uint32_t magic;         // 0x56464230 ("VFB0")
    uint32_t width;
    uint32_t height;
    uint32_t stride;        // stride in pixels
    uint32_t format;        // 1 = RGBA8888
    uint32_t frameSequence; // incremented on new frame
    uint32_t reserved[10];
};
#pragma pack(pop)

class VirtualFramebuffer {
public:
    static VirtualFramebuffer& getInstance();

    bool initialize(uint32_t width, uint32_t height, const std::string& sandboxDir);
    void close();

    bool isInitialized() const;
    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    uint32_t getStride() const { return mStride; }
    uint32_t getFrameSequence() const;

    const uint32_t* getPixelBuffer() const;
    uint32_t* getMutablePixelBuffer();
    void notifyFrameUpdated();
    void copyToTargetBuffer(uint32_t* dst, int dstWidth, int dstHeight, int dstStride) const;

    // Interactive Guest drawing operations
    void drawTouchCircle(int x, int y, uint32_t color, int radius);
    void clearScreen(uint32_t color);

private:
    VirtualFramebuffer() = default;
    ~VirtualFramebuffer();

    mutable std::mutex mBufferMutex;
    int mFd = -1;
    void* mMmapPtr = nullptr;
    size_t mTotalSize = 0;

    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mStride = 0;
    std::string mFilePath;
};

} // namespace gsi
