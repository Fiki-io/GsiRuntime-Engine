#include "include/keystore_bridge.h"
#include "include/property_service.h"
#include "include/logger.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>

#undef LOG_TAG
#define LOG_TAG "GSI_KeystoreBridge"

namespace gsi {

KeystoreBridge& KeystoreBridge::getInstance() {
    static KeystoreBridge instance;
    return instance;
}

bool KeystoreBridge::initialize(const std::string& sandboxDir) {
    if (mInitialized.load()) {
        return true;
    }

    mSandboxDir = sandboxDir;
    createMockDeviceNodes();
    seedDefaultKeysAndTemplates();
    registerSystemProperties();

    mInitialized.store(true);
    mBiometricsEnabled.store(true);
    LOGI("KeystoreBridge: Initialized with KeyMint v3 and CyberFP Biometrics stub");
    return true;
}

void KeystoreBridge::shutdown() {
    if (!mInitialized.load()) return;

    {
        std::lock_guard<std::mutex> lock(mKeystoreMutex);
        mKeyStore.clear();
        mEnrolledFingers.clear();
    }

    mInitialized.store(false);
    LOGI("KeystoreBridge: Shutdown completed");
}

void KeystoreBridge::createMockDeviceNodes() {
    std::string tmpDir = mSandboxDir + "/tmp";
    mkdir(tmpDir.c_str(), 0755);

    std::string fpNode = tmpDir + "/dev_fingerprint.raw";
    int fd = open(fpNode.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        std::string header = "CYBER_GSI_BIOMETRICS_MOCK_NODE_V1\nSENSOR_STATE=READY\n";
        write(fd, header.c_str(), header.size());
        close(fd);
    }
}

void KeystoreBridge::seedDefaultKeysAndTemplates() {
    std::lock_guard<std::mutex> lock(mKeystoreMutex);

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 1. Seed KeyMint Master Credentials
    mKeyStore["master_key_device_credential"] = {
        "master_key_device_credential", "AES-256", 256, false, static_cast<uint64_t>(nowMs)
    };
    mKeyStore["gatekeeper_password_token"] = {
        "gatekeeper_password_token", "HMAC-SHA256", 256, false, static_cast<uint64_t>(nowMs)
    };
    mKeyStore["user_lockscreen_pin_key"] = {
        "user_lockscreen_pin_key", "AES-256", 256, true, static_cast<uint64_t>(nowMs)
    };
    mKeyStore["system_cert_root_ca"] = {
        "system_cert_root_ca", "RSA-2048", 2048, false, static_cast<uint64_t>(nowMs)
    };

    // 2. Seed Default Biometric Enrolled Templates
    mEnrolledFingers[1] = {1, "CyberThumb Right", static_cast<uint64_t>(nowMs)};
    mEnrolledFingers[2] = {2, "CyberIndex Right", static_cast<uint64_t>(nowMs)};
    mEnrolledFingers[3] = {3, "CyberIndex Left", static_cast<uint64_t>(nowMs)};
}

void KeystoreBridge::registerSystemProperties() {
    auto& prop = PropertyService::getInstance();
    prop.setProperty("ro.hardware.fingerprint", "cyber_fp");
    prop.setProperty("ro.hardware.keystore", "cyber_keymint");
    prop.setProperty("ro.hardware.gatekeeper", "cyber_gatekeeper");
    prop.setProperty("persist.sys.fingerprint.enrolled", "true");
    prop.setProperty("vendor.security.keystore2.status", "ONLINE");
    prop.setProperty("vendor.security.keymint.version", "3");
    prop.setProperty("vendor.biometrics.fingerprint.status", "ONLINE");
}

bool KeystoreBridge::generateKey(const std::string& alias, const std::string& algorithm, int sizeBits, bool authRequired) {
    std::lock_guard<std::mutex> lock(mKeystoreMutex);

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    VirtualKeyEntry entry;
    entry.alias = alias;
    entry.algorithm = algorithm;
    entry.sizeBits = sizeBits;
    entry.authRequired = authRequired;
    entry.createdTimestampMs = static_cast<uint64_t>(nowMs);

    mKeyStore[alias] = entry;
    LOGI("KeystoreBridge: Key generated [%s] Alg: %s, Size: %d bits", alias.c_str(), algorithm.c_str(), sizeBits);
    return true;
}

bool KeystoreBridge::deleteKey(const std::string& alias) {
    std::lock_guard<std::mutex> lock(mKeystoreMutex);
    auto it = mKeyStore.find(alias);
    if (it != mKeyStore.end()) {
        mKeyStore.erase(it);
        LOGI("KeystoreBridge: Key deleted [%s]", alias.c_str());
        return true;
    }
    return false;
}

bool KeystoreBridge::enrollFingerprint(int fingerId, const std::string& name) {
    if (!mBiometricsEnabled.load()) return false;

    std::lock_guard<std::mutex> lock(mKeystoreMutex);
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    mEnrolledFingers[fingerId] = {fingerId, name, static_cast<uint64_t>(nowMs)};
    LOGI("KeystoreBridge: Enrolled finger #%d (%s)", fingerId, name.c_str());

    PropertyService::getInstance().setProperty("persist.sys.fingerprint.enrolled", "true");
    return true;
}

bool KeystoreBridge::removeFingerprint(int fingerId) {
    std::lock_guard<std::mutex> lock(mKeystoreMutex);
    auto it = mEnrolledFingers.find(fingerId);
    if (it != mEnrolledFingers.end()) {
        mEnrolledFingers.erase(it);
        LOGI("KeystoreBridge: Removed finger #%d", fingerId);
        if (mEnrolledFingers.empty()) {
            PropertyService::getInstance().setProperty("persist.sys.fingerprint.enrolled", "false");
        }
        return true;
    }
    return false;
}

BiometricAuthResult KeystoreBridge::authenticateFingerprint(bool shouldMatch) {
    BiometricAuthResult res;
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    res.timestampMs = static_cast<uint64_t>(nowMs);

    if (!mBiometricsEnabled.load()) {
        res.success = false;
        res.message = "ERROR: Biometric sensor is OFFLINE / DISABLED.";
        return res;
    }

    std::lock_guard<std::mutex> lock(mKeystoreMutex);
    mAuthAttempts++;

    if (mEnrolledFingers.empty()) {
        res.success = false;
        res.message = "REJECTED: No fingerprint templates enrolled.";
        return res;
    }

    if (shouldMatch) {
        mAuthSuccesses++;
        const auto& [id, templateData] = *mEnrolledFingers.begin();
        res.success = true;
        res.fingerId = id;
        std::ostringstream ss;
        ss << "AUTHENTICATED: Match Finger #" << id << " (" << templateData.name << ") [HAT Token Valid]";
        res.message = ss.str();
    } else {
        res.success = false;
        res.message = "REJECTED: Fingerprint not recognized (FPR Reject / Sensor Retry)";
    }

    return res;
}

void KeystoreBridge::toggleBiometrics(bool enabled) {
    mBiometricsEnabled.store(enabled);
    auto& prop = PropertyService::getInstance();
    prop.setProperty("vendor.biometrics.fingerprint.status", enabled ? "ONLINE" : "OFFLINE");
    LOGI("KeystoreBridge: Biometrics toggled to %s", enabled ? "ONLINE" : "OFFLINE");
}

std::string KeystoreBridge::getKeystoreStatsString() {
    std::lock_guard<std::mutex> lock(mKeystoreMutex);
    std::ostringstream ss;
    ss << "=== VIRTUAL KEYMINT & BIOMETRICS SUBSYSTEM ===\n"
       << "KeyMint Engine: ONLINE (KeyMint v3 / Keystore2 Virtual Provider)\n"
       << "Biometrics HAL: " << (mBiometricsEnabled.load() ? "ONLINE (Active)" : "OFFLINE (Disabled)") << "\n"
       << "Active Keystore Keys (" << mKeyStore.size() << "):\n";

    for (const auto& [alias, entry] : mKeyStore) {
        ss << "  * [" << alias << "] " << entry.algorithm
           << " (" << entry.sizeBits << "b)"
           << (entry.authRequired ? " [UserAuthReq]" : "") << "\n";
    }

    ss << "Enrolled Biometric Fingers (" << mEnrolledFingers.size() << "):\n";
    for (const auto& [id, finger] : mEnrolledFingers) {
        ss << "  * Finger #" << id << ": " << finger.name << "\n";
    }

    ss << "Biometric Auth Metrics: " << mAuthSuccesses << "/" << mAuthAttempts << " Successful Recognitions\n"
       << "Virtual Node: " << mSandboxDir << "/tmp/dev_fingerprint.raw";

    return ss.str();
}

} // namespace gsi
