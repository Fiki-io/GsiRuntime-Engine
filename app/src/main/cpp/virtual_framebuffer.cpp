#include "include/virtual_framebuffer.h"
#include "include/logger.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace gsi {

constexpr uint32_t VFB_MAGIC = 0x56464230; // "VFB0"

VirtualFramebuffer& VirtualFramebuffer::getInstance() {
    static VirtualFramebuffer instance;
    return instance;
}

VirtualFramebuffer::~VirtualFramebuffer() {
    close();
}

bool VirtualFramebuffer::initialize(uint32_t width, uint32_t height, const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    close();

    mWidth = width;
    mHeight = height;
    mStride = width;

    size_t headerSize = sizeof(VfbHeader);
    size_t pixelDataSize = static_cast<size_t>(mStride * mHeight * sizeof(uint32_t));
    mTotalSize = headerSize + pixelDataSize;

    mFilePath = sandboxDir + "/tmp/vfb0";

    mFd = open(mFilePath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (mFd < 0) {
        LOGE("Failed to create VFB shared file: %s", mFilePath.c_str());
        return false;
    }

    if (ftruncate(mFd, mTotalSize) != 0) {
        LOGE("Failed to truncate VFB shared file to %zu bytes", mTotalSize);
        ::close(mFd);
        mFd = -1;
        return false;
    }

    mMmapPtr = mmap(nullptr, mTotalSize, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, 0);
    if (mMmapPtr == MAP_FAILED) {
        LOGE("Failed to mmap VFB file");
        mMmapPtr = nullptr;
        ::close(mFd);
        mFd = -1;
        return false;
    }

    // Initialize Header
    VfbHeader* hdr = reinterpret_cast<VfbHeader*>(mMmapPtr);
    std::memset(hdr, 0, sizeof(VfbHeader));
    hdr->magic = VFB_MAGIC;
    hdr->width = mWidth;
    hdr->height = mHeight;
    hdr->stride = mStride;
    hdr->format = 1; // RGBA8888
    hdr->frameSequence = 1;

    // Clear background to sleek dark container color
    uint32_t* pixels = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(mMmapPtr) + sizeof(VfbHeader));
    for (size_t i = 0; i < (mStride * mHeight); ++i) {
        pixels[i] = 0xFF141721; // Dark navy background
    }

    LOGI("VirtualFramebuffer initialized: %ux%u (stride: %u, total: %zu bytes) at %s",
         mWidth, mHeight, mStride, mTotalSize, mFilePath.c_str());
    return true;
}

void VirtualFramebuffer::close() {
    if (mMmapPtr != nullptr && mMmapPtr != MAP_FAILED) {
        munmap(mMmapPtr, mTotalSize);
        mMmapPtr = nullptr;
    }
    if (mFd != -1) {
        ::close(mFd);
        mFd = -1;
    }
    mTotalSize = 0;
    mWidth = 0;
    mHeight = 0;
    mStride = 0;
}

bool VirtualFramebuffer::isInitialized() const {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    return (mMmapPtr != nullptr && mMmapPtr != MAP_FAILED);
}

uint32_t VirtualFramebuffer::getFrameSequence() const {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    if (!mMmapPtr) return 0;
    const VfbHeader* hdr = reinterpret_cast<const VfbHeader*>(mMmapPtr);
    return hdr->frameSequence;
}

const uint32_t* VirtualFramebuffer::getPixelBuffer() const {
    if (!mMmapPtr) return nullptr;
    return reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(mMmapPtr) + sizeof(VfbHeader));
}

void VirtualFramebuffer::copyToTargetBuffer(uint32_t* dst, int dstWidth, int dstHeight, int dstStride) const {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    if (!mMmapPtr || !dst) return;

    const uint32_t* src = getPixelBuffer();
    if (!src) return;

    // Nearest-neighbor / linear sample copy from VFB to destination display buffer
    for (int y = 0; y < dstHeight; ++y) {
        int srcY = (y * mHeight) / dstHeight;
        if (srcY >= static_cast<int>(mHeight)) srcY = mHeight - 1;

        const uint32_t* srcRow = src + (srcY * mStride);
        uint32_t* dstRow = dst + (y * dstStride);

        for (int x = 0; x < dstWidth; ++x) {
            int srcX = (x * mWidth) / dstWidth;
            if (srcX >= static_cast<int>(mWidth)) srcX = mWidth - 1;
            dstRow[x] = srcRow[srcX];
        }
    }
}

void VirtualFramebuffer::drawTouchCircle(int x, int y, uint32_t color, int radius) {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    if (!mMmapPtr) return;

    uint32_t* pixels = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(mMmapPtr) + sizeof(VfbHeader));
    int rSquared = radius * radius;

    int minX = std::max(0, x - radius);
    int maxX = std::min(static_cast<int>(mWidth) - 1, x + radius);
    int minY = std::max(0, y - radius);
    int maxY = std::min(static_cast<int>(mHeight) - 1, y + radius);

    for (int py = minY; py <= maxY; ++py) {
        int dy = py - y;
        uint32_t* row = pixels + (py * mStride);
        for (int px = minX; px <= maxX; ++px) {
            int dx = px - x;
            if ((dx * dx + dy * dy) <= rSquared) {
                row[px] = color;
            }
        }
    }

    VfbHeader* hdr = reinterpret_cast<VfbHeader*>(mMmapPtr);
    hdr->frameSequence++;
}

void VirtualFramebuffer::clearScreen(uint32_t color) {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    if (!mMmapPtr) return;

    uint32_t* pixels = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(mMmapPtr) + sizeof(VfbHeader));
    size_t count = mStride * mHeight;
    for (size_t i = 0; i < count; ++i) {
        pixels[i] = color;
    }

    VfbHeader* hdr = reinterpret_cast<VfbHeader*>(mMmapPtr);
    hdr->frameSequence++;
}

} // namespace gsi
