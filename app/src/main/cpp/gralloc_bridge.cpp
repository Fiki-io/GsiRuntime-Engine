#include "include/gralloc_bridge.h"
#include "include/virtual_framebuffer.h"
#include "include/logger.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cmath>
#include <chrono>
#include <sstream>

namespace gsi {

GrallocBridge& GrallocBridge::getInstance() {
    static GrallocBridge instance;
    return instance;
}

GrallocBridge::~GrallocBridge() {
    close();
}

bool GrallocBridge::initialize(const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mMutex);
    mSandboxDir = sandboxDir;
    LOGI("GrallocBridge initialized in sandbox: %s", sandboxDir.c_str());
    return true;
}

void GrallocBridge::close() {
    stopBootAnimation();

    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& buf : mActiveBuffers) {
        if (buf.mmapAddr != nullptr && buf.sizeInBytes > 0) {
            munmap(buf.mmapAddr, buf.sizeInBytes);
        }
        if (buf.fd >= 0) {
            ::close(buf.fd);
        }
    }
    mActiveBuffers.clear();
    LOGI("GrallocBridge closed and all virtual buffers released.");
}

int GrallocBridge::allocateBuffer(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, VirtualGraphicBuffer& outBuf) {
    std::lock_guard<std::mutex> lock(mMutex);

    uint32_t id = mNextBufferId++;
    uint32_t stride = (width + 15) & ~15; // 16-pixel align
    size_t size = stride * height * 4;   // RGBA8888 = 4 bytes per pixel

    std::string path = mSandboxDir + "/tmp/gralloc_" + std::to_string(id) + ".raw";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        LOGE("GrallocBridge: Failed to create backing file at %s", path.c_str());
        return -1;
    }

    if (ftruncate(fd, size) < 0) {
        LOGE("GrallocBridge: Failed to ftruncate backing file");
        ::close(fd);
        unlink(path.c_str());
        return -1;
    }

    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        LOGE("GrallocBridge: Failed to mmap virtual graphic buffer");
        ::close(fd);
        unlink(path.c_str());
        return -1;
    }

    std::memset(addr, 0, size);

    outBuf.id = id;
    outBuf.width = width;
    outBuf.height = height;
    outBuf.stride = stride;
    outBuf.format = format;
    outBuf.usage = usage;
    outBuf.sizeInBytes = size;
    outBuf.fd = fd;
    outBuf.mmapAddr = addr;

    mActiveBuffers.push_back(outBuf);

    LOGI("GrallocBridge: Allocated GraphicBuffer #%u (%ux%u, stride: %u, %zu bytes)",
         id, width, height, stride, size);
    return 0;
}

void GrallocBridge::freeBuffer(uint32_t bufferId) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto it = mActiveBuffers.begin(); it != mActiveBuffers.end(); ++it) {
        if (it->id == bufferId) {
            if (it->mmapAddr != nullptr) munmap(it->mmapAddr, it->sizeInBytes);
            if (it->fd >= 0) ::close(it->fd);
            std::string path = mSandboxDir + "/tmp/gralloc_" + std::to_string(bufferId) + ".raw";
            unlink(path.c_str());
            mActiveBuffers.erase(it);
            LOGI("GrallocBridge: Released GraphicBuffer #%u", bufferId);
            return;
        }
    }
}

size_t GrallocBridge::getActiveBufferCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mActiveBuffers.size();
}

std::string GrallocBridge::getGraphicsStats() const {
    std::lock_guard<std::mutex> lock(mMutex);
    std::ostringstream oss;
    oss << "Gralloc Buffers: " << mActiveBuffers.size()
        << " | VFB: 720x1280 RGBA8888"
        << " | BootAnim: " << (mBootAnimRunning.load() ? "ACTIVE (60 FPS)" : "IDLE");
    return oss.str();
}

void GrallocBridge::startBootAnimation() {
    if (mBootAnimRunning.load()) return;

    if (mBootAnimThread.joinable()) {
        mBootAnimThread.join();
    }

    mBootAnimRunning.store(true);
    mBootAnimThread = std::thread(&GrallocBridge::bootAnimationLoop, this);
    LOGI("GrallocBridge: Android 60 FPS Boot Animation thread started");
}

void GrallocBridge::stopBootAnimation() {
    if (!mBootAnimRunning.load()) return;

    mBootAnimRunning.store(false);
    if (mBootAnimThread.joinable()) {
        mBootAnimThread.join();
    }
    LOGI("GrallocBridge: Android Boot Animation stopped");
}

void GrallocBridge::bootAnimationLoop() {
    auto startTime = std::chrono::steady_clock::now();

    while (mBootAnimRunning.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        auto& vfb = VirtualFramebuffer::getInstance();
        if (vfb.isInitialized()) {
            uint32_t width = vfb.getWidth();
            uint32_t height = vfb.getHeight();
            uint32_t stride = vfb.getStride();
            const uint32_t* pixelPtr = vfb.getPixelBuffer();

            if (pixelPtr != nullptr) {
                auto now = std::chrono::steady_clock::now();
                float timeSec = std::chrono::duration<float>(now - startTime).count();

                // Cast const away for direct VFB rendering in boot animation mode
                auto* writablePixels = const_cast<uint32_t*>(pixelPtr);
                renderBootAnimationFrame(writablePixels, width, height, stride, timeSec);
            }
        }

        // Maintain ~60 FPS
        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart).count();
        if (elapsedMs < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsedMs));
        }
    }
}

void GrallocBridge::renderBootAnimationFrame(uint32_t* targetPixels, uint32_t width, uint32_t height, uint32_t stride, float timeSec) {
    if (!targetPixels || width == 0 || height == 0) return;

    int cx = static_cast<int>(width) / 2;
    int cy = static_cast<int>(height) / 2 - 40;

    // Pulse factor: 0.8 to 1.2
    float pulse = 0.85f + 0.15f * std::sin(timeSec * 3.0f);

    // Shimmer wave moving diagonally across screen: pos from -200 to 1400
    float sweep = std::fmod(timeSec * 450.0f, static_cast<float>(width + height + 200)) - 100.0f;

    for (int y = 0; y < static_cast<int>(height); ++y) {
        float dy = static_cast<float>(y - cy);

        for (int x = 0; x < static_cast<int>(width); ++x) {
            float dx = static_cast<float>(x - cx);

            // 1. Dark cyber space background with subtle radial aura
            float distFromCenter = std::sqrt(dx * dx + dy * dy);
            float aura = std::max(0.0f, 1.0f - distFromCenter / 450.0f) * pulse;

            uint8_t bgR = static_cast<uint8_t>(10.0f + 5.0f * aura);
            uint8_t bgG = static_cast<uint8_t>(13.0f + 25.0f * aura);
            uint8_t bgB = static_cast<uint8_t>(20.0f + 45.0f * aura);

            // 2. Android Bugdroid Geometry
            bool isRobot = false;

            // Dome Head (semi-circle): dy between -110 and -25, radius 75
            float headDist = std::sqrt(dx * dx + (dy + 25.0f) * (dy + 25.0f));
            if (headDist <= 75.0f && (dy + 25.0f) <= 0.0f) {
                // Eyes: cutouts at (cx - 30, cy - 65) and (cx + 30, cy - 65)
                float leftEyeDist = std::sqrt((dx + 30.0f) * (dx + 30.0f) + (dy + 65.0f) * (dy + 65.0f));
                float rightEyeDist = std::sqrt((dx - 30.0f) * (dx - 30.0f) + (dy + 65.0f) * (dy + 65.0f));
                if (leftEyeDist > 8.0f && rightEyeDist > 8.0f) {
                    isRobot = true;
                }
            }

            // Antennae: angled bars from head
            // Left antenna: from (-30, -95) pointing up-left
            // Right antenna: from (30, -95) pointing up-right
            if (dy >= -135.0f && dy <= -95.0f) {
                float leftAntX = -30.0f - ((-95.0f - dy) * 0.6f);
                float rightAntX = 30.0f + ((-95.0f - dy) * 0.6f);
                if (std::abs(dx - leftAntX) <= 4.0f || std::abs(dx - rightAntX) <= 4.0f) {
                    isRobot = true;
                }
            }

            // Torso Body: rounded rect dx in [-75, 75], dy in [-15, 75]
            if (std::abs(dx) <= 75.0f && dy >= -15.0f && dy <= 75.0f) {
                isRobot = true;
            }

            // Left & Right Arms: pills
            float leftArmDx = dx + 95.0f;
            float rightArmDx = dx - 95.0f;
            if (dy >= -10.0f && dy <= 60.0f) {
                if (std::abs(leftArmDx) <= 12.0f || std::abs(rightArmDx) <= 12.0f) {
                    isRobot = true;
                }
            }

            // Left & Right Legs: pills below torso
            float leftLegDx = dx + 30.0f;
            float rightLegDx = dx - 30.0f;
            if (dy >= 80.0f && dy <= 125.0f) {
                if (std::abs(leftLegDx) <= 14.0f || std::abs(rightLegDx) <= 14.0f) {
                    isRobot = true;
                }
            }

            if (isRobot) {
                // Shimmer wave calculation: distance to diagonal sweep line
                float sweepDist = std::abs((x + y) - sweep);
                float shimmerIntensity = std::max(0.0f, 1.0f - sweepDist / 60.0f);

                // Base robot color: Neon Android Cyber Cyan-Green (#00E676 / #00E5FF)
                uint8_t r = static_cast<uint8_t>(0 + 220 * shimmerIntensity);
                uint8_t g = static_cast<uint8_t>(230 + 25 * shimmerIntensity);
                uint8_t b = static_cast<uint8_t>(118 + 137 * shimmerIntensity);
                uint8_t a = 255;

                targetPixels[y * stride + x] = (a << 24) | (b << 16) | (g << 8) | r;
            } else {
                // Background pixel
                targetPixels[y * stride + x] = (255 << 24) | (bgB << 16) | (bgG << 8) | bgR;
            }
        }
    }

    // 3. Render Cyber Pulse Bar at bottom
    int barY = cy + 220;
    int barWidth = 260;
    int barLeft = cx - barWidth / 2;
    int barRight = cx + barWidth / 2;
    float barProgress = std::fmod(timeSec * 0.7f, 1.0f);
    int activeX = barLeft + static_cast<int>(barProgress * barWidth);

    for (int y = barY; y < barY + 4; ++y) {
        if (y < 0 || y >= static_cast<int>(height)) continue;
        for (int x = barLeft; x < barRight; ++x) {
            float dist = std::abs(x - activeX);
            uint8_t glow = static_cast<uint8_t>(std::max(0.0f, 1.0f - dist / 30.0f) * 255.0f);
            uint8_t r = glow;
            uint8_t g = std::max<uint8_t>(glow, 60);
            uint8_t b = std::max<uint8_t>(glow, 90);
            targetPixels[y * stride + x] = (255 << 24) | (b << 16) | (g << 8) | r;
        }
    }
}

} // namespace gsi
