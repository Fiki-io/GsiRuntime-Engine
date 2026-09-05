#pragma once

#include <string>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>

namespace gsi {

struct BleDevice {
    std::string name;
    std::string address;
    int rssi;
    std::string type;
};

struct BluetoothState {
    bool isEnabled = true;
    std::string name = "CyberGSI-Bluetooth";
    std::string address = "00:1A:7D:DA:71:13";
    std::string version = "5.2 (BLE / Dual Mode)";
    std::string scanMode = "CONNECTABLE_DISCOVERABLE";
    std::vector<BleDevice> discoveredDevices;
};

class BluetoothBridge {
public:
    static BluetoothBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool toggleBluetooth(bool enabled);
    bool startBleScan();

    BluetoothState getState();
    std::string getBluetoothStatsString();

private:
    BluetoothBridge() = default;
    ~BluetoothBridge();

    void updateProperties();
    void vhciWorkerLoop();

    std::mutex mBtMutex;
    std::string mSandboxDir;
    bool mInitialized = false;

    BluetoothState mState;
    std::atomic<bool> mVhciRunning{false};
    std::thread mVhciThread;
};

} // namespace gsi
