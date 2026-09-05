#include "include/input_bridge.h"
#include "include/logger.h"

#include <linux/input.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace gsi {

InputBridge& InputBridge::getInstance() {
    static InputBridge instance;
    return instance;
}

InputBridge::~InputBridge() {
    close();
}

bool InputBridge::initialize(const std::string& sandboxDir, uint32_t screenWidth, uint32_t screenHeight) {
    std::lock_guard<std::mutex> lock(mInputMutex);
    close();

    mScreenWidth = screenWidth;
    mScreenHeight = screenHeight;
    mFifoPath = sandboxDir + "/tmp/touch_event";

    // Create FIFO if it does not exist
    unlink(mFifoPath.c_str());
    if (mkfifo(mFifoPath.c_str(), 0666) != 0) {
        LOGW("mkfifo failed for %s, falling back to regular file", mFifoPath.c_str());
    }

    // Open O_RDWR | O_NONBLOCK so write() won't block or trigger SIGPIPE
    mFifoFd = open(mFifoPath.c_str(), O_RDWR | O_NONBLOCK);
    if (mFifoFd < 0) {
        LOGE("Failed to open touch event pipe: %s", mFifoPath.c_str());
        return false;
    }

    LOGI("InputBridge initialized: %ux%u (pipe: %s)", mScreenWidth, mScreenHeight, mFifoPath.c_str());
    return true;
}

void InputBridge::close() {
    if (mFifoFd != -1) {
        ::close(mFifoFd);
        mFifoFd = -1;
    }
}

void InputBridge::writeEvent(uint16_t type, uint16_t code, int32_t value) {
    if (mFifoFd < 0) return;

    struct input_event ev{};
    struct timeval tv{};
    gettimeofday(&tv, nullptr);

    ev.time = tv;
    ev.type = type;
    ev.code = code;
    ev.value = value;

    write(mFifoFd, &ev, sizeof(ev));
}

void InputBridge::writeSync() {
    writeEvent(EV_SYN, SYN_REPORT, 0);
}

void InputBridge::injectTouchEvent(int action, float normX, float normY, int pointerId) {
    std::lock_guard<std::mutex> lock(mInputMutex);
    if (mFifoFd < 0) return;

    int32_t pixelX = static_cast<int32_t>(std::clamp(normX, 0.0f, 1.0f) * (mScreenWidth - 1));
    int32_t pixelY = static_cast<int32_t>(std::clamp(normY, 0.0f, 1.0f) * (mScreenHeight - 1));

    switch (action) {
        case 0: // ACTION_DOWN
            writeEvent(EV_ABS, ABS_MT_TRACKING_ID, pointerId);
            writeEvent(EV_ABS, ABS_MT_POSITION_X, pixelX);
            writeEvent(EV_ABS, ABS_MT_POSITION_Y, pixelY);
            writeEvent(EV_KEY, BTN_TOUCH, 1);
            writeSync();
            break;

        case 2: // ACTION_MOVE
            writeEvent(EV_ABS, ABS_MT_POSITION_X, pixelX);
            writeEvent(EV_ABS, ABS_MT_POSITION_Y, pixelY);
            writeSync();
            break;

        case 1: // ACTION_UP
        case 3: // ACTION_CANCEL
            writeEvent(EV_ABS, ABS_MT_TRACKING_ID, -1);
            writeEvent(EV_KEY, BTN_TOUCH, 0);
            writeSync();
            break;

        default:
            break;
    }
}

} // namespace gsi
