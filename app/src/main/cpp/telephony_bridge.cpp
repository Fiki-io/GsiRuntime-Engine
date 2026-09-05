#include "include/telephony_bridge.h"
#include "include/property_service.h"

#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sstream>

#define LOG_TAG "GSI_TelephonyBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace gsi {

TelephonyBridge& TelephonyBridge::getInstance() {
    static TelephonyBridge instance;
    return instance;
}

TelephonyBridge::~TelephonyBridge() {
    shutdown();
}

bool TelephonyBridge::initialize(const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mTelephonyMutex);
    mSandboxDir = sandboxDir;

    LOGI("TelephonyBridge: Initializing Virtual Telephony & Wi-Fi Bridge in %s", mSandboxDir.c_str());

    std::string socketDir = mSandboxDir + "/dev/socket";
    mkdir(socketDir.c_str(), 0775);

    updateProperties();
    startSocketServers();

    mInitialized = true;
    LOGI("TelephonyBridge: Subsystem initialized (SIM: %s, Wi-Fi: %s)",
         mSimState.carrierName.c_str(), mWifiState.ssid.c_str());
    return true;
}

void TelephonyBridge::shutdown() {
    std::lock_guard<std::mutex> lock(mTelephonyMutex);
    if (!mInitialized) return;

    mRildRunning.store(false);
    mRildDebugRunning.store(false);
    mWpaRunning.store(false);

    if (mRildThread.joinable()) mRildThread.join();
    if (mRildDebugThread.joinable()) mRildDebugThread.join();
    if (mWpaThread.joinable()) mWpaThread.join();

    mInitialized = false;
    LOGI("TelephonyBridge: Subsystem shutdown successfully.");
}

bool TelephonyBridge::toggleSim(bool enabled) {
    std::lock_guard<std::mutex> lock(mTelephonyMutex);
    mSimState.isInserted = enabled;
    if (enabled) {
        mSimState.carrierName = "CyberGSI 5G";
        mSimState.signalBars = 5;
        mSimState.signalDbm = -65;
        mSimState.dataState = "CONNECTED";
        mSimState.networkType = "NR_SA";
    } else {
        mSimState.carrierName = "No SIM";
        mSimState.signalBars = 0;
        mSimState.signalDbm = -120;
        mSimState.dataState = "DISCONNECTED";
        mSimState.networkType = "NONE";
    }
    updateProperties();
    LOGI("TelephonyBridge: SIM state changed (inserted=%d, carrier=%s)",
         enabled ? 1 : 0, mSimState.carrierName.c_str());
    return true;
}

bool TelephonyBridge::toggleWifi(bool connected) {
    std::lock_guard<std::mutex> lock(mTelephonyMutex);
    mWifiState.isConnected = connected;
    updateProperties();
    LOGI("TelephonyBridge: Wi-Fi state changed (connected=%d, SSID=%s)",
         connected ? 1 : 0, mWifiState.ssid.c_str());
    return true;
}

SimState TelephonyBridge::getSimState() {
    std::lock_guard<std::mutex> lock(mTelephonyMutex);
    return mSimState;
}

WifiState TelephonyBridge::getWifiState() {
    std::lock_guard<std::mutex> lock(mTelephonyMutex);
    return mWifiState;
}

std::string TelephonyBridge::getTelephonyStatsString() {
    std::lock_guard<std::mutex> lock(mTelephonyMutex);
    std::ostringstream oss;
    oss << "=== Cellular & Telephony (RIL) ===\n"
        << "SIM State: " << (mSimState.isInserted ? "INSERTED / READY" : "NO SIM (Emergency Only)") << "\n"
        << "Operator: " << mSimState.carrierName << " (" << mSimState.mcc << "-" << mSimState.mnc << ")\n"
        << "Signal: " << mSimState.signalBars << "/5 bars (" << mSimState.signalDbm << " dBm)\n"
        << "Network: " << mSimState.networkType << " | Data: " << mSimState.dataState << "\n"
        << "IMEI: " << mSimState.imei << " | IMSI: " << mSimState.imsi << "\n"
        << "\n=== Wireless LAN (Wi-Fi) ===\n"
        << "Wi-Fi State: " << (mWifiState.isConnected ? "CONNECTED" : "DISCONNECTED") << "\n"
        << "SSID: " << mWifiState.ssid << " (" << mWifiState.bssid << ")\n"
        << "IP: " << mWifiState.ipAddress << " (GW: " << mWifiState.gateway << ")\n"
        << "Speed: " << mWifiState.linkSpeedMbps << " Mbps | RSSI: " << mWifiState.rssiDbm << " dBm";
    return oss.str();
}

void TelephonyBridge::updateProperties() {
    auto& prop = PropertyService::getInstance();

    if (mSimState.isInserted) {
        prop.setProperty("gsm.sim.state", "READY");
        prop.setProperty("gsm.sim.operator.alpha", mSimState.carrierName);
        prop.setProperty("gsm.sim.operator.numeric", mSimState.mcc + mSimState.mnc);
        prop.setProperty("gsm.sim.operator.iso-country", "us");
        prop.setProperty("gsm.operator.alpha", mSimState.carrierName);
        prop.setProperty("gsm.operator.numeric", mSimState.mcc + mSimState.mnc);
        prop.setProperty("gsm.operator.isroaming", "false");
        prop.setProperty("gsm.network.type", mSimState.networkType);
        prop.setProperty("ril.ready", "true");
        prop.setProperty("ro.telephony.default_network", "9");
        prop.setProperty("persist.sys.sim.state", "READY");
        prop.setProperty("persist.sys.sim.carrier", mSimState.carrierName);
        prop.setProperty("persist.sys.sim.bars", std::to_string(mSimState.signalBars));
        prop.setProperty("persist.sys.sim.imei", mSimState.imei);
    } else {
        prop.setProperty("gsm.sim.state", "ABSENT");
        prop.setProperty("gsm.sim.operator.alpha", "");
        prop.setProperty("gsm.sim.operator.numeric", "");
        prop.setProperty("gsm.operator.alpha", "Emergency Calls Only");
        prop.setProperty("gsm.network.type", "NONE");
        prop.setProperty("persist.sys.sim.state", "ABSENT");
        prop.setProperty("persist.sys.sim.carrier", "No SIM");
        prop.setProperty("persist.sys.sim.bars", "0");
    }

    prop.setProperty("wifi.interface", "wlan0");
    prop.setProperty("init.svc.wpa_supplicant", "running");
    if (mWifiState.isConnected) {
        prop.setProperty("persist.sys.wifi.state", "CONNECTED");
        prop.setProperty("persist.sys.wifi.ssid", mWifiState.ssid);
        prop.setProperty("persist.sys.wifi.ip", mWifiState.ipAddress);
        prop.setProperty("persist.sys.wifi.speed", std::to_string(mWifiState.linkSpeedMbps) + " Mbps");
        prop.setProperty("persist.sys.wifi.rssi", std::to_string(mWifiState.rssiDbm) + " dBm");
    } else {
        prop.setProperty("persist.sys.wifi.state", "DISCONNECTED");
        prop.setProperty("persist.sys.wifi.ssid", "");
        prop.setProperty("persist.sys.wifi.ip", "");
        prop.setProperty("persist.sys.wifi.speed", "0 Mbps");
        prop.setProperty("persist.sys.wifi.rssi", "-100 dBm");
    }
}

void TelephonyBridge::rildSocketLoop(const std::string& socketPath, std::atomic<bool>& runningFlag) {
    unlink(socketPath.c_str());

    int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd < 0) {
        LOGE("TelephonyBridge: Failed to create socket %s", socketPath.c_str());
        return;
    }

    // Set non-blocking mode
    int flags = fcntl(serverFd, F_GETFL, 0);
    fcntl(serverFd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("TelephonyBridge: Failed to bind socket %s", socketPath.c_str());
        close(serverFd);
        return;
    }

    chmod(socketPath.c_str(), 0666);
    listen(serverFd, 5);
    LOGI("TelephonyBridge: Socket listener online at %s", socketPath.c_str());

    while (runningFlag.load()) {
        struct pollfd pfd;
        pfd.fd = serverFd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500); // 500ms poll timeout
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int clientFd = accept(serverFd, nullptr, nullptr);
            if (clientFd >= 0) {
                LOGI("TelephonyBridge: Client connected on %s", socketPath.c_str());
                // Handle basic keepalive handshake
                char buf[128];
                ssize_t n = read(clientFd, buf, sizeof(buf));
                if (n > 0) {
                    // Send ACK
                    const char* ack = "\x00\x00\x00\x04\x00\x00\x00\x00"; // Generic RIL success packet
                    write(clientFd, ack, 8);
                }
                close(clientFd);
            }
        }
    }

    close(serverFd);
    unlink(socketPath.c_str());
    LOGI("TelephonyBridge: Socket listener %s stopped.", socketPath.c_str());
}

void TelephonyBridge::startSocketServers() {
    mRildRunning.store(true);
    mRildDebugRunning.store(true);
    mWpaRunning.store(true);

    std::string rildPath = mSandboxDir + "/dev/socket/rild";
    std::string rildDebugPath = mSandboxDir + "/dev/socket/rild-debug";
    std::string wpaPath = mSandboxDir + "/dev/socket/wpa_wlan0";

    mRildThread = std::thread([this, rildPath]() {
        rildSocketLoop(rildPath, mRildRunning);
    });

    mRildDebugThread = std::thread([this, rildDebugPath]() {
        rildSocketLoop(rildDebugPath, mRildDebugRunning);
    });

    mWpaThread = std::thread([this, wpaPath]() {
        rildSocketLoop(wpaPath, mWpaRunning);
    });
}

} // namespace gsi
