#pragma once

#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>

namespace gsi {

struct CameraDevice {
    int id = 0;                     // 0 = Back, 1 = Front
    std::string name;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string nodePath;
    int nodeFd = -1;
    bool isStreaming = false;
    uint64_t frameCount = 0;
    float currentFps = 0.0f;
};

class CameraBridge {
public:
    static CameraBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool startStream(int cameraId, uint32_t width = 1280, uint32_t height = 720);
    void stopStream(int cameraId);

    uint64_t getFrameCount(int cameraId);
    std::string getCameraStatsString();

private:
    CameraBridge() = default;
    ~CameraBridge();

    void generatorLoop(int cameraId);
    void generateTestPattern(uint8_t* buffer, uint32_t width, uint32_t height, uint64_t frameIndex);

    std::mutex mCameraMutex;
    std::string mSandboxDir;
    bool mInitialized = false;

    CameraDevice mCameras[2];
    std::atomic<bool> mGeneratorRunning[2]{false, false};
    std::thread mGeneratorThreads[2];
};

} // namespace gsi
