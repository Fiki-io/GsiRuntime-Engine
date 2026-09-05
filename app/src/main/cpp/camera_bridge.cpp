#include "include/camera_bridge.h"
#include "include/logger.h"
#include "include/property_service.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace gsi {

namespace {

// Standard 8-color SMPTE color bars (RGBA values)
const uint32_t SMPTE_COLORS[8] = {
    0xFFFFFFFF, // White
    0xFF00FFFF, // Yellow (ABGR: R=FF, G=FF, B=00)
    0xFFFFFF00, // Cyan   (ABGR: R=00, G=FF, B=FF)
    0xFF00FF00, // Green
    0xFFFF00FF, // Magenta
    0xFF0000FF, // Red
    0xFFFF0000, // Blue
    0xFF000000  // Black
};

} // anonymous namespace

CameraBridge& CameraBridge::getInstance() {
    static CameraBridge instance;
    return instance;
}

CameraBridge::~CameraBridge() {
    shutdown();
}

bool CameraBridge::initialize(const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mCameraMutex);

    mSandboxDir = sandboxDir;

    // Camera 0: Back camera (HD 1280x720)
    mCameras[0].id = 0;
    mCameras[0].name = "Virtual Back Camera (HAL3)";
    mCameras[0].width = 1280;
    mCameras[0].height = 720;
    mCameras[0].nodePath = sandboxDir + "/tmp/v4l2_video0.raw";
    mCameras[0].frameCount = 0;
    mCameras[0].isStreaming = false;

    // Camera 1: Front camera (VGA 640x480)
    mCameras[1].id = 1;
    mCameras[1].name = "Virtual Front Camera (HAL3)";
    mCameras[1].width = 640;
    mCameras[1].height = 480;
    mCameras[1].nodePath = sandboxDir + "/tmp/v4l2_video1.raw";
    mCameras[1].frameCount = 0;
    mCameras[1].isStreaming = false;

    for (int i = 0; i < 2; ++i) {
        // Pre-create backing node file
        int fd = open(mCameras[i].nodePath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            size_t frameSize = mCameras[i].width * mCameras[i].height * 4; // RGBA
            ftruncate(fd, static_cast<off_t>(frameSize));
            close(fd);
        }
    }

    mInitialized = true;

    // Write initial test pattern to Back camera node
    size_t backSize = mCameras[0].width * mCameras[0].height * 4;
    std::vector<uint8_t> initialBuf(backSize);
    generateTestPattern(initialBuf.data(), mCameras[0].width, mCameras[0].height, 0);

    int fd0 = open(mCameras[0].nodePath.c_str(), O_WRONLY);
    if (fd0 >= 0) {
        write(fd0, initialBuf.data(), backSize);
        close(fd0);
    }

    PropertyService::getInstance().setProperty("camera.gsi.status", "ready");
    PropertyService::getInstance().setProperty("camera.gsi.count", "2");

    LOGI("CameraBridge: Initialized V4L2 virtual cameras (/dev/video0 -> %s, /dev/video1 -> %s)",
         mCameras[0].nodePath.c_str(), mCameras[1].nodePath.c_str());

    return true;
}

void CameraBridge::shutdown() {
    stopStream(0);
    stopStream(1);

    std::lock_guard<std::mutex> lock(mCameraMutex);
    mInitialized = false;
}

bool CameraBridge::startStream(int cameraId, uint32_t width, uint32_t height) {
    if (cameraId < 0 || cameraId > 1) return false;

    stopStream(cameraId);

    std::lock_guard<std::mutex> lock(mCameraMutex);
    if (!mInitialized) return false;

    if (width > 0 && height > 0) {
        mCameras[cameraId].width = width;
        mCameras[cameraId].height = height;
    }

    mCameras[cameraId].isStreaming = true;
    mGeneratorRunning[cameraId].store(true);

    mGeneratorThreads[cameraId] = std::thread(&CameraBridge::generatorLoop, this, cameraId);

    PropertyService::getInstance().setProperty("camera.active", std::to_string(cameraId));
    PropertyService::getInstance().setProperty("camera.gsi.status", "streaming");

    LOGI("CameraBridge: Started 30 FPS stream on Camera %d (%ux%u)",
         cameraId, mCameras[cameraId].width, mCameras[cameraId].height);

    return true;
}

void CameraBridge::stopStream(int cameraId) {
    if (cameraId < 0 || cameraId > 1) return;

    if (mGeneratorRunning[cameraId].load()) {
        mGeneratorRunning[cameraId].store(false);
        if (mGeneratorThreads[cameraId].joinable()) {
            mGeneratorThreads[cameraId].join();
        }
    }

    std::lock_guard<std::mutex> lock(mCameraMutex);
    mCameras[cameraId].isStreaming = false;
    mCameras[cameraId].currentFps = 0.0f;
    PropertyService::getInstance().setProperty("camera.gsi.status", "idle");

    LOGI("CameraBridge: Stopped stream on Camera %d", cameraId);
}

uint64_t CameraBridge::getFrameCount(int cameraId) {
    if (cameraId < 0 || cameraId > 1) return 0;
    std::lock_guard<std::mutex> lock(mCameraMutex);
    return mCameras[cameraId].frameCount;
}

void CameraBridge::generateTestPattern(uint8_t* buffer, uint32_t width, uint32_t height, uint64_t frameIndex) {
    if (!buffer) return;

    uint32_t* pixels = reinterpret_cast<uint32_t*>(buffer);
    uint32_t barWidth = width / 8;
    uint32_t bottomBarY = (height * 3) / 4;

    // Moving scan indicator across the bottom
    uint32_t scanX = (frameIndex * 8) % width;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t idx = y * width + x;

            if (y < bottomBarY) {
                // Top 75%: 8 vertical color bars
                uint32_t barIdx = std::min(x / barWidth, 7u);
                pixels[idx] = SMPTE_COLORS[barIdx];
            } else {
                // Bottom 25%: Cyber gradient with moving indicator
                uint32_t grad = ((x * 255) / width) & 0xFF;
                // Dark background
                uint32_t col = 0xFF000000 | (grad << 8) | (grad / 2);

                // Moving scan bar (16px wide)
                if (x >= scanX && x < scanX + 16) {
                    col = 0xFF00E5FF; // Neon Cyan scan indicator
                }
                pixels[idx] = col;
            }
        }
    }
}

void CameraBridge::generatorLoop(int cameraId) {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string nodePath;

    {
        std::lock_guard<std::mutex> lock(mCameraMutex);
        width = mCameras[cameraId].width;
        height = mCameras[cameraId].height;
        nodePath = mCameras[cameraId].nodePath;
    }

    size_t bufferSize = width * height * 4;
    std::vector<uint8_t> frameBuffer(bufferSize);

    int fd = open(nodePath.c_str(), O_RDWR);
    if (fd < 0) {
        LOGE("CameraBridge: Failed to open node %s", nodePath.c_str());
        return;
    }

    uint64_t localFrames = 0;
    auto lastFpsTime = std::chrono::steady_clock::now();
    uint64_t lastFpsFrames = 0;

    while (mGeneratorRunning[cameraId].load()) {
        auto startTime = std::chrono::steady_clock::now();

        // Generate synthetic test frame
        generateTestPattern(frameBuffer.data(), width, height, localFrames);

        // Write frame to backing node
        pwrite(fd, frameBuffer.data(), bufferSize, 0);

        localFrames++;

        // Measure FPS every 1 second
        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - lastFpsTime).count();
        if (elapsedSec >= 1.0) {
            float fps = static_cast<float>((localFrames - lastFpsFrames) / elapsedSec);
            lastFpsTime = now;
            lastFpsFrames = localFrames;

            std::lock_guard<std::mutex> lock(mCameraMutex);
            mCameras[cameraId].frameCount = localFrames;
            mCameras[cameraId].currentFps = fps;
        }

        // Sleep to maintain ~30 FPS (33.3 ms per frame)
        auto frameDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime
        );
        int sleepMs = 33 - static_cast<int>(frameDuration.count());
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }

    close(fd);
}

std::string CameraBridge::getCameraStatsString() {
    std::lock_guard<std::mutex> lock(mCameraMutex);
    std::ostringstream oss;
    oss << "=== VIRTUAL CAMERA & V4L2 HAL SUBSYSTEM ===\n";
    for (int i = 0; i < 2; ++i) {
        oss << "Camera " << i << " [" << mCameras[i].name << "]\n"
            << "  Resolution: " << mCameras[i].width << "x" << mCameras[i].height << " (RGBA/NV21)\n"
            << "  Status    : " << (mCameras[i].isStreaming ? "STREAMING" : "IDLE") << "\n"
            << "  FPS       : " << mCameras[i].currentFps << "\n"
            << "  Frames    : " << mCameras[i].frameCount << "\n"
            << "  V4L2 Node : " << mCameras[i].nodePath << "\n";
    }
    oss << "============================================";
    return oss.str();
}

} // namespace gsi
