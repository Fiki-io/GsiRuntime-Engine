#pragma once

#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>

namespace gsi {

struct SimState {
    bool isInserted = true;
    std::string carrierName = "CyberGSI 5G";
    std::string mcc = "310";
    std::string mnc = "260";
    std::string imei = "860123456789012";
    std::string imsi = "310260123456789";
    std::string phoneNumber = "+1-555-0199";
    int signalBars = 5;         // 0 to 5
    int signalDbm = -65;        // -120 to -50
    std::string networkType = "NR_SA"; // 5G Standalone
    std::string dataState = "CONNECTED";
    bool isRoaming = false;
};

struct WifiState {
    bool isConnected = true;
    std::string ssid = "GSI-Virtual-WiFi";
    std::string bssid = "02:00:00:11:22:33";
    std::string ipAddress = "192.168.1.150";
    std::string gateway = "192.168.1.1";
    int linkSpeedMbps = 866;
    int rssiDbm = -42;
    int frequencyMhz = 5180;
};

class TelephonyBridge {
public:
    static TelephonyBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool toggleSim(bool enabled);
    bool toggleWifi(bool connected);

    SimState getSimState();
    WifiState getWifiState();
    std::string getTelephonyStatsString();

private:
    TelephonyBridge() = default;
    ~TelephonyBridge();

    void startSocketServers();
    void rildSocketLoop(const std::string& socketPath, std::atomic<bool>& runningFlag);
    void updateProperties();

    std::mutex mTelephonyMutex;
    std::string mSandboxDir;
    bool mInitialized = false;

    SimState mSimState;
    WifiState mWifiState;

    std::atomic<bool> mRildRunning{false};
    std::atomic<bool> mRildDebugRunning{false};
    std::atomic<bool> mWpaRunning{false};

    std::thread mRildThread;
    std::thread mRildDebugThread;
    std::thread mWpaThread;
};

} // namespace gsi
