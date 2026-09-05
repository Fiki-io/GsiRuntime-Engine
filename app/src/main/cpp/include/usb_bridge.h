#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>

namespace gsi {

struct AdbPacket {
    uint32_t command; // e.g. A_SYNC, A_CNXN, A_OPEN, A_OKAY, A_CLSE, A_WRTE
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_check;
    uint32_t magic;
};

class UsbBridge {
public:
    static UsbBridge& getInstance();

    bool initialize(const std::string& sandboxDir, int tcpPort = 5555);
    void shutdown();

    bool startAdbServer();
    void stopAdbServer();

    bool isAdbServerRunning() const { return mAdbServerRunning.load(); }
    int getAdbPort() const { return mTcpPort; }

    std::string simulateLocalAdbConnection();
    std::string getUsbStatsString();
    const std::string& getSandboxDir() const { return mSandboxDir; }

private:
    UsbBridge() = default;
    ~UsbBridge() { shutdown(); }

    void createMockUsbNodes();
    void registerSystemProperties();
    void serverLoop();

    std::string mSandboxDir;
    int mTcpPort = 5555;
    std::atomic<bool> mInitialized{false};
    std::atomic<bool> mAdbServerRunning{false};
    int mServerSocketFd = -1;

    std::thread mServerThread;
    mutable std::mutex mUsbMutex;

    uint32_t mActiveConnections = 0;
    uint64_t mTotalAdbCommands = 0;
    uint64_t mTotalBytesTransferred = 0;
};

} // namespace gsi
