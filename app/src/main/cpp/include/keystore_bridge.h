#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

namespace gsi {

struct VirtualKeyEntry {
    std::string alias;
    std::string algorithm; // "AES-256", "RSA-2048", "HMAC-SHA256", "EC-P256"
    int sizeBits = 256;
    bool authRequired = false;
    uint64_t createdTimestampMs = 0;
};

struct FingerprintTemplate {
    int fingerId = 0;
    std::string name;
    uint64_t enrolledTimestampMs = 0;
};

struct BiometricAuthResult {
    bool success = false;
    int fingerId = 0;
    std::string message;
    uint64_t timestampMs = 0;
};

class KeystoreBridge {
public:
    static KeystoreBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool generateKey(const std::string& alias, const std::string& algorithm, int sizeBits = 256, bool authRequired = false);
    bool deleteKey(const std::string& alias);

    bool enrollFingerprint(int fingerId, const std::string& name);
    bool removeFingerprint(int fingerId);
    BiometricAuthResult authenticateFingerprint(bool shouldMatch);

    void toggleBiometrics(bool enabled);
    bool isBiometricsEnabled() const { return mBiometricsEnabled.load(); }

    std::string getKeystoreStatsString();
    const std::string& getSandboxDir() const { return mSandboxDir; }

private:
    KeystoreBridge() = default;
    ~KeystoreBridge() { shutdown(); }

    void createMockDeviceNodes();
    void seedDefaultKeysAndTemplates();
    void registerSystemProperties();

    std::string mSandboxDir;
    std::atomic<bool> mInitialized{false};
    std::atomic<bool> mBiometricsEnabled{true};

    std::mutex mKeystoreMutex;
    std::map<std::string, VirtualKeyEntry> mKeyStore;
    std::map<int, FingerprintTemplate> mEnrolledFingers;

    uint32_t mAuthAttempts = 0;
    uint32_t mAuthSuccesses = 0;
};

} // namespace gsi
