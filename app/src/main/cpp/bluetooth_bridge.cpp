#include "include/bluetooth_bridge.h"
#include "include/property_service.h"

#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sstream>
#include <chrono>

#define LOG_TAG "GSI_BluetoothBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace gsi {

BluetoothBridge& BluetoothBridge::getInstance() {
    static BluetoothBridge instance;
    return instance;
}

BluetoothBridge::~BluetoothBridge() {
    shutdown();
}

bool BluetoothBridge::initialize(const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mBtMutex);
    mSandboxDir = sandboxDir;

    LOGI("BluetoothBridge: Initializing Virtual Bluetooth Subsystem at %s", mSandboxDir.c_str());

    std::string tmpDir = mSandboxDir + "/tmp";
    mkdir(tmpDir.c_str(), 0775);

    // 1. Create dummy /dev/rfkill backing node
    std::string rfkillPath = tmpDir + "/dev_rfkill.raw";
    int rfkillFd = open(rfkillPath.c_str(), O_RDWR | O_CREAT, 0666);
    if (rfkillFd >= 0) {
        // rfkill event: type 2 (bluetooth), op 0, soft 0 (unblocked), hard 0 (unblocked)
        uint8_t rfkillEvent[8] = {0, 0, 0, 0, 2, 0, 0, 0};
        write(rfkillFd, rfkillEvent, sizeof(rfkillEvent));
        close(rfkillFd);
    }

    // 2. Create dummy /dev/vhci backing node
    std::string vhciPath = tmpDir + "/dev_vhci.raw";
    int vhciFd = open(vhciPath.c_str(), O_RDWR | O_CREAT, 0666);
    if (vhciFd >= 0) {
        close(vhciFd);
    }

    // 3. Seed default discovered devices
    mState.discoveredDevices = {
        {"CyberWatch Ultra", "E4:5F:01:23:45:67", -52, "BLE Smartwatch"},
        {"CyberBuds Pro ANC", "C8:2B:96:78:9A:BC", -64, "Bluetooth Audio"},
        {"GSI Beacon Hub", "A0:C9:A0:DE:F0:12", -78, "BLE Proximity Beacon"}
    };

    updateProperties();

    // 4. Start VHCI worker thread
    mVhciRunning.store(true);
    mVhciThread = std::thread(&BluetoothBridge::vhciWorkerLoop, this);

    mInitialized = true;
    LOGI("BluetoothBridge: Virtual Bluetooth HAL Subsystem online (%s | %s)",
         mState.name.c_str(), mState.address.c_str());
    return true;
}

void BluetoothBridge::shutdown() {
    std::lock_guard<std::mutex> lock(mBtMutex);
    if (!mInitialized) return;

    mVhciRunning.store(false);
    if (mVhciThread.joinable()) {
        mVhciThread.join();
    }

    mInitialized = false;
    LOGI("BluetoothBridge: Virtual Bluetooth bridge shutdown successfully.");
}

bool BluetoothBridge::toggleBluetooth(bool enabled) {
    std::lock_guard<std::mutex> lock(mBtMutex);
    mState.isEnabled = enabled;
    updateProperties();
    LOGI("BluetoothBridge: Bluetooth state toggled to %s", enabled ? "ENABLED (ON)" : "DISABLED (OFF)");
    return true;
}

bool BluetoothBridge::startBleScan() {
    std::lock_guard<std::mutex> lock(mBtMutex);
    if (!mState.isEnabled) {
        LOGE("BluetoothBridge: Cannot scan while Bluetooth is disabled");
        return false;
    }

    // Refresh RSSI metrics
    mState.discoveredDevices = {
        {"CyberWatch Ultra", "E4:5F:01:23:45:67", -48, "BLE Smartwatch"},
        {"CyberBuds Pro ANC", "C8:2B:96:78:9A:BC", -61, "Bluetooth Audio"},
        {"GSI Beacon Hub", "A0:C9:A0:DE:F0:12", -74, "BLE Proximity Beacon"},
        {"CyberKey Fob", "70:89:CC:11:22:33", -58, "BLE HID Security Key"}
    };

    LOGI("BluetoothBridge: BLE Scan completed. %zu devices found nearby.",
         mState.discoveredDevices.size());
    return true;
}

BluetoothState BluetoothBridge::getState() {
    std::lock_guard<std::mutex> lock(mBtMutex);
    return mState;
}

std::string BluetoothBridge::getBluetoothStatsString() {
    std::lock_guard<std::mutex> lock(mBtMutex);
    std::ostringstream oss;
    oss << "=== Virtual Bluetooth Subsystem ===\n"
        << "Adapter State: " << (mState.isEnabled ? "STATE_ON (Enabled)" : "STATE_OFF (Disabled)") << "\n"
        << "Adapter Name: " << mState.name << "\n"
        << "MAC Address: " << mState.address << "\n"
        << "HCI Node: /dev/vhci (Virtual Controller)\n"
        << "Version: " << mState.version << "\n"
        << "Scan Mode: " << mState.scanMode << "\n"
        << "\n=== Discovered Nearby Peripherals (" << mState.discoveredDevices.size() << ") ===\n";

    for (size_t i = 0; i < mState.discoveredDevices.size(); ++i) {
        const auto& d = mState.discoveredDevices[i];
        oss << "[" << (i + 1) << "] " << d.name << " (" << d.address << ")\n"
            << "    Type: " << d.type << " | Signal: " << d.rssi << " dBm\n";
    }

    return oss.str();
}

void BluetoothBridge::updateProperties() {
    auto& prop = PropertyService::getInstance();

    if (mState.isEnabled) {
        prop.setProperty("persist.sys.bluetooth.enabled", "true");
        prop.setProperty("persist.sys.bluetooth.state", "STATE_ON");
        prop.setProperty("persist.sys.bluetooth.name", mState.name);
        prop.setProperty("persist.sys.bluetooth.address", mState.address);
        prop.setProperty("bluetooth.status", "enabled");
        prop.setProperty("bluetooth.profile.a2dp.sink.enabled", "true");
        prop.setProperty("bluetooth.profile.hfp.hf.enabled", "true");
        prop.setProperty("bluetooth.profile.pan.panu.enabled", "true");
    } else {
        prop.setProperty("persist.sys.bluetooth.enabled", "false");
        prop.setProperty("persist.sys.bluetooth.state", "STATE_OFF");
        prop.setProperty("bluetooth.status", "disabled");
    }
}

void BluetoothBridge::vhciWorkerLoop() {
    std::string vhciPath = mSandboxDir + "/tmp/dev_vhci.raw";

    while (mVhciRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Check if vhci backing node exists; ensure node integrity
        if (access(vhciPath.c_str(), F_OK) != 0) {
            int fd = open(vhciPath.c_str(), O_RDWR | O_CREAT, 0666);
            if (fd >= 0) close(fd);
        }
    }
}

} // namespace gsi
