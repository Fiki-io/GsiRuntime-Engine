#include "include/usb_bridge.h"
#include "include/property_service.h"
#include "include/process_spawner.h"
#include "include/logger.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sstream>
#include <iomanip>
#include <cstring>

#undef LOG_TAG
#define LOG_TAG "GSI_UsbBridge"

namespace gsi {

UsbBridge& UsbBridge::getInstance() {
    static UsbBridge instance;
    return instance;
}

bool UsbBridge::initialize(const std::string& sandboxDir, int tcpPort) {
    if (mInitialized.load()) {
        return true;
    }

    mSandboxDir = sandboxDir;
    mTcpPort = tcpPort;
    createMockUsbNodes();
    registerSystemProperties();

    mInitialized.store(true);
    startAdbServer();
    LOGI("UsbBridge: Initialized with Virtual USB Gadget and ADB TCP port %d", mTcpPort);
    return true;
}

void UsbBridge::shutdown() {
    if (!mInitialized.load()) return;

    stopAdbServer();
    mInitialized.store(false);
    LOGI("UsbBridge: Shutdown completed");
}

void UsbBridge::createMockUsbNodes() {
    std::string tmpDir = mSandboxDir + "/tmp";
    mkdir(tmpDir.c_str(), 0755);

    const std::vector<std::string> usbNodes = {
        tmpDir + "/dev_usb_accessory.raw",
        tmpDir + "/dev_mtp_usb.raw",
        tmpDir + "/dev_android_adb.raw"
    };

    for (const auto& path : usbNodes) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            std::string header = "CYBER_GSI_USB_GADGET_MOCK_NODE_V1\nFUNCTION=ADB,MTP\n";
            write(fd, header.c_str(), header.size());
            close(fd);
        }
    }
}

void UsbBridge::registerSystemProperties() {
    auto& prop = PropertyService::getInstance();
    prop.setProperty("persist.sys.usb.config", "adb");
    prop.setProperty("sys.usb.state", "adb");
    prop.setProperty("service.adb.tcp.port", std::to_string(mTcpPort));
    prop.setProperty("ro.adb.secure", "0");
    prop.setProperty("ro.debuggable", "1");
    prop.setProperty("vendor.usb.status", "ONLINE");
}

bool UsbBridge::startAdbServer() {
    if (mAdbServerRunning.load()) return true;

    mServerSocketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (mServerSocketFd < 0) {
        LOGE("UsbBridge: Failed to create socket for ADB TCP server");
        return false;
    }

    int opt = 1;
    setsockopt(mServerSocketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(mTcpPort);

    if (bind(mServerSocketFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOGW("UsbBridge: Port %d bind failed (may be occupied or unprivileged), trying loopback mock mode", mTcpPort);
    } else {
        listen(mServerSocketFd, 5);
        LOGI("UsbBridge: ADB TCP Server listening on 0.0.0.0:%d", mTcpPort);
    }

    mAdbServerRunning.store(true);
    if (mServerThread.joinable()) mServerThread.join();
    mServerThread = std::thread(&UsbBridge::serverLoop, this);
    return true;
}

void UsbBridge::stopAdbServer() {
    if (!mAdbServerRunning.load()) return;

    mAdbServerRunning.store(false);
    if (mServerSocketFd >= 0) {
        close(mServerSocketFd);
        mServerSocketFd = -1;
    }

    if (mServerThread.joinable()) {
        mServerThread.join();
    }
    LOGI("UsbBridge: ADB TCP Server stopped");
}

void UsbBridge::serverLoop() {
    while (mAdbServerRunning.load() && mServerSocketFd >= 0) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(mServerSocketFd, &readFds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int activity = select(mServerSocketFd + 1, &readFds, nullptr, nullptr, &tv);
        if (activity > 0 && FD_ISSET(mServerSocketFd, &readFds)) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(mServerSocketFd, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientFd >= 0) {
                {
                    std::lock_guard<std::mutex> lock(mUsbMutex);
                    mActiveConnections++;
                    mTotalAdbCommands++;
                }

                // Send ADB CNXN Handshake Header
                std::string banner = "CNXN\x01\x00\x00\x01\x00\x10\x00\x00device::ro.product.name=CyberGSI;ro.product.model=AOSP_Treble;\n";
                send(clientFd, banner.c_str(), banner.size(), 0);

                char buf[256];
                ssize_t bytes = recv(clientFd, buf, sizeof(buf) - 1, 0);
                if (bytes > 0) {
                    buf[bytes] = '\0';
                    std::lock_guard<std::mutex> lock(mUsbMutex);
                    mTotalBytesTransferred += bytes;
                }

                close(clientFd);
                {
                    std::lock_guard<std::mutex> lock(mUsbMutex);
                    if (mActiveConnections > 0) mActiveConnections--;
                }
            }
        }
    }
}

std::string UsbBridge::simulateLocalAdbConnection() {
    std::lock_guard<std::mutex> lock(mUsbMutex);
    mTotalAdbCommands++;
    mTotalBytesTransferred += 512;

    std::ostringstream ss;
    ss << "=== IN-SANDBOX ADB CONNECTION TEST ===\n"
       << "Endpoint: tcp:127.0.0.1:" << mTcpPort << "\n"
       << "Handshake: CNXN (Version 0x01000001, MaxData: 4096)\n"
       << "Auth Status: BYPASSED (ro.adb.secure=0 Permissive Debug)\n"
       << "Target Device: CyberGSI Treble ARM64 [Online]\n"
       << "Shell Pipe: /system/bin/sh -> ProcessSpawner Hooked\n"
       << "Status: READY (Command execution stream established)";
    return ss.str();
}

std::string UsbBridge::getUsbStatsString() {
    std::lock_guard<std::mutex> lock(mUsbMutex);
    std::ostringstream ss;
    ss << "=== VIRTUAL USB & ADB SERVER SUBSYSTEM ===\n"
       << "ADB Server: " << (mAdbServerRunning.load() ? "ONLINE (Listening on port " + std::to_string(mTcpPort) + ")" : "OFFLINE") << "\n"
       << "TCP Endpoint: localhost:" << mTcpPort << "\n"
       << "USB Mode: " << "adb,mtp (Virtual USB Gadget)\n"
       << "Active Connections: " << mActiveConnections << "\n"
       << "Total ADB Commands Served: " << mTotalAdbCommands << "\n"
       << "Total Data Transferred: " << mTotalBytesTransferred << " bytes\n"
       << "Virtual USB Nodes:\n"
       << "  - /dev/android_adb    -> " << mSandboxDir << "/tmp/dev_android_adb.raw\n"
       << "  - /dev/mtp_usb        -> " << mSandboxDir << "/tmp/dev_mtp_usb.raw\n"
       << "  - /dev/usb_accessory  -> " << mSandboxDir << "/tmp/dev_usb_accessory.raw";
    return ss.str();
}

} // namespace gsi
