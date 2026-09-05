#include "include/display_pipeline.h"
#include "include/virtual_framebuffer.h"
#include "include/logger.h"

#include <chrono>
#include <cmath>
#include <algorithm>

namespace gsi {

DisplayPipeline& DisplayPipeline::getInstance() {
    static DisplayPipeline instance;
    return instance;
}

DisplayPipeline::DisplayPipeline() = default;

DisplayPipeline::~DisplayPipeline() {
    stopTestRender();
    clearSurface();
}

void DisplayPipeline::setSurface(JNIEnv* env, jobject surface) {
    std::lock_guard<std::mutex> lock(mWindowMutex);

    if (mNativeWindow != nullptr) {
        LOGI("Releasing previous ANativeWindow");
        ANativeWindow_release(mNativeWindow);
        mNativeWindow = nullptr;
    }

    if (surface != nullptr) {
        mNativeWindow = ANativeWindow_fromSurface(env, surface);
        if (mNativeWindow != nullptr) {
            int width = ANativeWindow_getWidth(mNativeWindow);
            int height = ANativeWindow_getHeight(mNativeWindow);
            int32_t format = ANativeWindow_getFormat(mNativeWindow);
            LOGI("Acquired ANativeWindow: %dx%d, format: %d", width, height, format);

            ANativeWindow_setBuffersGeometry(mNativeWindow, 0, 0, WINDOW_FORMAT_RGBA_8888);
        } else {
            LOGE("Failed to acquire ANativeWindow from Surface");
        }
    }
}

void DisplayPipeline::clearSurface() {
    std::lock_guard<std::mutex> lock(mWindowMutex);
    if (mNativeWindow != nullptr) {
        ANativeWindow_release(mNativeWindow);
        mNativeWindow = nullptr;
        LOGI("Cleared ANativeWindow");
    }
}

bool DisplayPipeline::isSurfaceReady() const {
    std::lock_guard<std::mutex> lock(mWindowMutex);
    return mNativeWindow != nullptr;
}

void DisplayPipeline::startTestRender() {
    if (mIsRunning.load()) {
        LOGW("Display render loop is already running");
        return;
    }

    mIsRunning.store(true);
    mFrameCount.store(0);
    mRenderThread = std::thread(&DisplayPipeline::renderLoop, this);
    LOGI("DisplayPipeline render thread started");
}

void DisplayPipeline::stopTestRender() {
    if (!mIsRunning.load()) {
        return;
    }

    mIsRunning.store(false);
    if (mRenderThread.joinable()) {
        mRenderThread.join();
    }
    LOGI("DisplayPipeline render thread stopped");
}

void DisplayPipeline::renderLoop() {
    uint32_t step = 0;

    while (mIsRunning.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mWindowMutex);
            if (mNativeWindow != nullptr) {
                ANativeWindow_Buffer buffer;
                if (ANativeWindow_lock(mNativeWindow, &buffer, nullptr) == 0) {
                    uint32_t* pixels = static_cast<uint32_t*>(buffer.bits);
                    int stride = buffer.stride;
                    int width = buffer.width;
                    int height = buffer.height;

                    if (mUseVfbMode.load() && VirtualFramebuffer::getInstance().isInitialized()) {
                        // VFB Mode: Blit pixels directly from guest shared memory!
                        VirtualFramebuffer::getInstance().copyToTargetBuffer(pixels, width, height, stride);
                    } else {
                        // Default Test Gradient mode
                        float phase = static_cast<float>(step) * 0.05f;
                        int scanY = static_cast<int>((std::sin(phase * 0.5f) * 0.5f + 0.5f) * height);

                        for (int y = 0; y < height; ++y) {
                            float yNorm = static_cast<float>(y) / static_cast<float>(height);
                            bool isScanline = (std::abs(y - scanY) <= 3);

                            for (int x = 0; x < width; ++x) {
                                float xNorm = static_cast<float>(x) / static_cast<float>(width);

                                uint8_t r = static_cast<uint8_t>(20.0f + 30.0f * std::sin(xNorm * 3.14f + phase));
                                uint8_t g = static_cast<uint8_t>(30.0f + 60.0f * yNorm);
                                uint8_t b = static_cast<uint8_t>(70.0f + 80.0f * std::cos(xNorm * 3.14f + phase));
                                uint8_t a = 255;

                                if (isScanline) {
                                    r = 0;
                                    g = 240;
                                    b = 255;
                                }

                                pixels[y * stride + x] = (a << 24) | (b << 16) | (g << 8) | r;
                            }
                        }
                    }

                    ANativeWindow_unlockAndPost(mNativeWindow);
                    mFrameCount.fetch_add(1);
                    step++;
                }
            }
        }

        // Maintain ~60 FPS
        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart);
        if (elapsed.count() < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsed.count()));
        }
    }
}

} // namespace gsi
