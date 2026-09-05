#pragma once

#include <cstdint>
#include <string>
#include <mutex>

namespace gsi {

class InputBridge {
public:
    static InputBridge& getInstance();

    bool initialize(const std::string& sandboxDir, uint32_t screenWidth, uint32_t screenHeight);
    void close();

    // action: 0 = ACTION_DOWN, 1 = ACTION_UP, 2 = ACTION_MOVE
    void injectTouchEvent(int action, float normX, float normY, int pointerId);

    // Hardware Navigation & Virtual Input Keys (Pilar 4)
    // Linux standard keycodes (<linux/input.h>)
    static constexpr int KEY_CODE_BACK       = 158; // KEY_BACK
    static constexpr int KEY_CODE_HOME       = 172; // KEY_HOMEPAGE (Android Home)
    static constexpr int KEY_CODE_RECENTS    = 580; // KEY_APPSELECT (Android Recents / Overview)
    static constexpr int KEY_CODE_POWER      = 116; // KEY_POWER
    static constexpr int KEY_CODE_VOLUME_UP  = 115; // KEY_VOLUMEUP
    static constexpr int KEY_CODE_VOLUME_DOWN = 114; // KEY_VOLUMEDOWN

    // action: 1 = DOWN (press), 0 = UP (release)
    void injectKeyEvent(int keyCode, int action);
    void injectKeyClick(int keyCode);
    std::string getInputStatsString();

private:
    InputBridge() = default;
    ~InputBridge();

    void writeEvent(uint16_t type, uint16_t code, int32_t value);
    void writeSync();

    std::mutex mInputMutex;
    int mFifoFd = -1;
    std::string mFifoPath;
    uint32_t mScreenWidth = 720;
    uint32_t mScreenHeight = 1280;

    uint64_t mTotalTouchEvents = 0;
    uint64_t mTotalKeyEvents = 0;
};

} // namespace gsi

