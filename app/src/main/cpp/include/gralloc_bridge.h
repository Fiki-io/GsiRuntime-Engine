#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

namespace gsi {

struct VirtualGraphicBuffer {
    uint32_t id = 0;
    uint32_t width = 720;
    uint32_t height = 1280;
    uint32_t stride = 720;
    uint32_t format = 1; // HAL_PIXEL_FORMAT_RGBA_8888
    uint32_t usage = 0;
    size_t sizeInBytes = 0;
    int fd = -1;
    void* mmapAddr = nullptr;
};

class GrallocBridge {
public:
    static GrallocBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void close();

    // Virtual buffer allocation & management
    int allocateBuffer(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, VirtualGraphicBuffer& outBuf);
    void freeBuffer(uint32_t bufferId);

    // Boot Splash & Animation Compositor (60 FPS)
    void startBootAnimation();
    void stopBootAnimation();
    bool isBootAnimationActive() const { return mBootAnimRunning.load(); }

    // Statistics
    size_t getActiveBufferCount() const;
    std::string getGraphicsStats() const;

private:
    GrallocBridge() = default;
    ~GrallocBridge();

    GrallocBridge(const GrallocBridge&) = delete;
    GrallocBridge& operator=(const GrallocBridge&) = delete;

    void bootAnimationLoop();
    void renderBootAnimationFrame(uint32_t* targetPixels, uint32_t width, uint32_t height, uint32_t stride, float timeSec);

    mutable std::mutex mMutex;
    std::string mSandboxDir;
    uint32_t mNextBufferId{1};
    std::vector<VirtualGraphicBuffer> mActiveBuffers;

    std::atomic<bool> mBootAnimRunning{false};
    std::thread mBootAnimThread;
};

} // namespace gsi
