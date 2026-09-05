#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace gsi {

struct NetworkStats {
    std::string primaryDns;
    std::string secondaryDns;
    std::string ipv6Dns;
    std::string hostname;
    bool isNetdRunning{false};
    uint64_t totalQueriesProcessed{0};
};

class NetworkBridge {
public:
    static NetworkBridge& getInstance();

    // Initializes network configuration files ($SANDBOX/etc/resolv.conf, /etc/hosts),
    // syncs system properties in PropertyService, and starts the virtual netd socket listener.
    bool initialize(const std::string& sandboxDir,
                    const std::string& primaryDns = "8.8.8.8",
                    const std::string& secondaryDns = "1.1.1.1");

    void shutdown();
    bool isRunning() const { return mIsRunning.load(); }

    // Diagnostic DNS resolution and TCP round-trip latency tester
    std::string testConnectivity(const std::string& host = "google.com", int port = 443);

    // Returns a formatted status string of network bridge components
    std::string getNetworkStatsString() const;
    NetworkStats getStats() const;

private:
    NetworkBridge();
    ~NetworkBridge();

    NetworkBridge(const NetworkBridge&) = delete;
    NetworkBridge& operator=(const NetworkBridge&) = delete;

    void provisionFiles();
    void registerSystemProperties();
    void netdWorkerLoop();
    void handleNetdClient(int clientFd);

    std::string mSandboxDir;
    std::string mPrimaryDns{"8.8.8.8"};
    std::string mSecondaryDns{"1.1.1.1"};
    std::string mIpv6Dns{"2001:4860:4860::8888"};
    std::string mHostname{"android-gsi"};
    std::string mNetdSocketPath;

    int mNetdServerFd{-1};
    std::atomic<bool> mIsRunning{false};
    std::thread mNetdThread;

    mutable std::mutex mStatsMutex;
    uint64_t mQueryCounter{0};
};

} // namespace gsi
