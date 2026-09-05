#pragma once

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <thread>
#include <mutex>

namespace gsi {

class DisplayPipeline {
public:
    static DisplayPipeline& getInstance();

    void setSurface(JNIEnv* env, jobject surface);
    void clearSurface();
    bool isSurfaceReady() const;

    void startTestRender();
    void stopTestRender();

    void setVfbMode(bool enable) { mUseVfbMode.store(enable); }
    bool isVfbMode() const { return mUseVfbMode.load(); }

    int getRenderedFrames() const { return mFrameCount.load(); }

private:
    DisplayPipeline();
    ~DisplayPipeline();

    void renderLoop();

    ANativeWindow* mNativeWindow = nullptr;
    mutable std::mutex mWindowMutex;

    std::atomic<bool> mIsRunning{false};
    std::atomic<bool> mUseVfbMode{false};
    std::atomic<int> mFrameCount{0};
    std::thread mRenderThread;
};

} // namespace gsi
