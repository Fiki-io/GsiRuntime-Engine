#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace gsi {

struct VirtualGpuStats {
    std::string eglVendor;
    std::string eglVersion;
    std::string glRenderer;
    std::string glVersion;
    std::string glExtensions;
    uint32_t activeContexts = 0;
    uint64_t totalFramesRendered = 0;
    bool isHardwareAccelerated = true;
};

class GpuBridge {
public:
    static GpuBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    std::string runGpuSelfTest();
    std::string getGpuStatsString();

    bool renderTestTriangleToVfb();
    bool clearVfbColor(uint32_t colorRgba);

    bool isInitialized() const { return mInitialized.load(); }
    const std::string& getSandboxDir() const { return mSandboxDir; }

private:
    GpuBridge() = default;
    ~GpuBridge() { shutdown(); }

    void createMockGpuNodes();
    void registerSystemProperties();

    std::string mSandboxDir;
    std::atomic<bool> mInitialized{false};
    std::mutex mGpuMutex;

    VirtualGpuStats mStats;
};

} // namespace gsi
