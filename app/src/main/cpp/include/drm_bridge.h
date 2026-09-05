#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>

namespace gsi {

struct DrmCryptoSession {
    std::string sessionId;
    std::string schemeUuid;
    std::string schemeName;
    uint64_t openedTimestampMs = 0;
    uint32_t decryptedPackets = 0;
    uint64_t decryptedBytes = 0;
};

class DrmBridge {
public:
    static DrmBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool isSchemeSupported(const std::string& uuidStr);
    bool openSession(const std::string& uuidStr, std::string& outSessionId);
    bool closeSession(const std::string& sessionId);

    bool decryptSample(const std::string& sessionId,
                       const std::vector<uint8_t>& encData,
                       std::vector<uint8_t>& outDecData);

    std::string runCryptoSelfTest();
    std::string getDrmStatsString();
    void toggleDrm(bool enabled);

    bool isEnabled() const { return mEnabled.load(); }
    const std::string& getSandboxDir() const { return mSandboxDir; }

    static constexpr const char* CLEARKEY_UUID = "e660e10f-26ac-4428-9b77-98f653650536";
    static constexpr const char* WIDEVINE_UUID = "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";

private:
    DrmBridge() = default;
    ~DrmBridge() { shutdown(); }

    void createMockDeviceNodes();
    void registerSystemProperties();

    std::string mSandboxDir;
    std::atomic<bool> mInitialized{false};
    std::atomic<bool> mEnabled{true};

    std::mutex mSessionMutex;
    std::map<std::string, DrmCryptoSession> mActiveSessions;
    uint64_t mSessionCounter = 0;
    uint64_t mTotalDecryptedBytes = 0;
};

} // namespace gsi
