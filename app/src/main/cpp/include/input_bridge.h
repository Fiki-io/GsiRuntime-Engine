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
};

} // namespace gsi
