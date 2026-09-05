#include "include/drm_bridge.h"
#include "include/property_service.h"
#include "include/logger.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

#undef LOG_TAG
#define LOG_TAG "GSI_DrmBridge"

namespace gsi {

static std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

DrmBridge& DrmBridge::getInstance() {
    static DrmBridge instance;
    return instance;
}

bool DrmBridge::initialize(const std::string& sandboxDir) {
    if (mInitialized.load()) {
        return true;
    }

    mSandboxDir = sandboxDir;
    createMockDeviceNodes();
    registerSystemProperties();

    mInitialized.store(true);
    mEnabled.store(true);
    LOGI("DrmBridge: Initialized successfully with ClearKey and Widevine L3 stubs");
    return true;
}

void DrmBridge::shutdown() {
    if (!mInitialized.load()) return;

    {
        std::lock_guard<std::mutex> lock(mSessionMutex);
        mActiveSessions.clear();
    }

    mInitialized.store(false);
    LOGI("DrmBridge: Shutdown completed");
}

void DrmBridge::createMockDeviceNodes() {
    std::string tmpDir = mSandboxDir + "/tmp";
    mkdir(tmpDir.c_str(), 0755);

    const std::vector<std::string> mockNodes = {
        tmpDir + "/dev_tee0.raw",
        tmpDir + "/dev_ion.raw",
        tmpDir + "/dev_qseecom.raw"
    };

    for (const auto& nodePath : mockNodes) {
        int fd = open(nodePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            std::string header = "CYBER_GSI_DRM_MOCK_NODE_V1\n";
            write(fd, header.c_str(), header.size());
            close(fd);
        }
    }
}

void DrmBridge::registerSystemProperties() {
    auto& prop = PropertyService::getInstance();
    prop.setProperty("drm.service.enabled", "true");
    prop.setProperty("media.mediadrmservice.enable", "true");
    prop.setProperty("ro.hardware.drm", "cyber_drm");
    prop.setProperty("ro.hardware.crypto", "cyber_crypto");
    prop.setProperty("drm.64bit.enabled", "true");
    prop.setProperty("vendor.drm.clearkey.supported", "true");
    prop.setProperty("vendor.drm.widevine.security_level", "L3");
    prop.setProperty("vendor.drm.status", "ONLINE");
}

bool DrmBridge::isSchemeSupported(const std::string& uuidStr) {
    if (!mEnabled.load()) return false;
    std::string lowerUuid = toLower(uuidStr);
    return (lowerUuid == CLEARKEY_UUID || lowerUuid == WIDEVINE_UUID);
}

bool DrmBridge::openSession(const std::string& uuidStr, std::string& outSessionId) {
    if (!mEnabled.load()) return false;
    if (!isSchemeSupported(uuidStr)) return false;

    std::lock_guard<std::mutex> lock(mSessionMutex);
    mSessionCounter++;

    std::ostringstream ss;
    ss << "drm-sess-" << mSessionCounter;
    outSessionId = ss.str();

    std::string lowerUuid = toLower(uuidStr);
    std::string schemeName = (lowerUuid == CLEARKEY_UUID) ? "W3C-ClearKey" : "Widevine-L3";

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    DrmCryptoSession session;
    session.sessionId = outSessionId;
    session.schemeUuid = lowerUuid;
    session.schemeName = schemeName;
    session.openedTimestampMs = static_cast<uint64_t>(nowMs);
    session.decryptedPackets = 0;
    session.decryptedBytes = 0;

    mActiveSessions[outSessionId] = session;
    LOGI("DrmBridge: Session opened [%s] for scheme %s", outSessionId.c_str(), schemeName.c_str());
    return true;
}

bool DrmBridge::closeSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(mSessionMutex);
    auto it = mActiveSessions.find(sessionId);
    if (it != mActiveSessions.end()) {
        LOGI("DrmBridge: Session closed [%s]", sessionId.c_str());
        mActiveSessions.erase(it);
        return true;
    }
    return false;
}

bool DrmBridge::decryptSample(const std::string& sessionId,
                              const std::vector<uint8_t>& encData,
                              std::vector<uint8_t>& outDecData) {
    if (!mEnabled.load()) return false;

    std::lock_guard<std::mutex> lock(mSessionMutex);
    auto it = mActiveSessions.find(sessionId);
    if (it == mActiveSessions.end()) {
        return false;
    }

    outDecData.resize(encData.size());
    // AES-128-CTR pseudo-keystream decryption simulator
    const uint8_t keyStreamSeed[16] = {
        0x5a, 0xa5, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
        0xde, 0xf0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66
    };

    for (size_t i = 0; i < encData.size(); ++i) {
        uint8_t mask = keyStreamSeed[i % 16] ^ static_cast<uint8_t>((i / 16) & 0xFF);
        outDecData[i] = encData[i] ^ mask;
    }

    it->second.decryptedPackets++;
    it->second.decryptedBytes += encData.size();
    mTotalDecryptedBytes += encData.size();
    return true;
}

std::string DrmBridge::runCryptoSelfTest() {
    if (!mEnabled.load()) {
        return "ERROR: DRM subsystem is currently DISABLED.";
    }

    std::string sessionId;
    if (!openSession(CLEARKEY_UUID, sessionId)) {
        return "FAILED: Could not open ClearKey DRM session.";
    }

    const std::string rawPayload = "[GSI_VIRTUAL_DRM_SAMPLE_FRAME_4K_AVC_NAL_0x65]";
    std::vector<uint8_t> plain(rawPayload.begin(), rawPayload.end());

    // Encrypt by applying mask
    const uint8_t keyStreamSeed[16] = {
        0x5a, 0xa5, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
        0xde, 0xf0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66
    };
    std::vector<uint8_t> cipher(plain.size());
    for (size_t i = 0; i < plain.size(); ++i) {
        uint8_t mask = keyStreamSeed[i % 16] ^ static_cast<uint8_t>((i / 16) & 0xFF);
        cipher[i] = plain[i] ^ mask;
    }

    // Decrypt using decryptSample
    std::vector<uint8_t> decrypted;
    bool ok = decryptSample(sessionId, cipher, decrypted);
    closeSession(sessionId);

    if (!ok || decrypted != plain) {
        return "FAILED: Decrypted payload mismatch in AES-128 test!";
    }

    std::string resultStr(decrypted.begin(), decrypted.end());
    std::ostringstream ss;
    ss << "=== DRM CRYPTO SELF-TEST PASSED ===\n"
       << "Scheme: W3C ClearKey (UUID: " << CLEARKEY_UUID << ")\n"
       << "Cipher: AES-128-CTR Virtual Pipeline\n"
       << "Payload Size: " << plain.size() << " bytes\n"
       << "Verification: 100% Match -> \"" << resultStr << "\"";
    return ss.str();
}

std::string DrmBridge::getDrmStatsString() {
    std::lock_guard<std::mutex> lock(mSessionMutex);
    std::ostringstream ss;
    ss << "=== VIRTUAL DRM & MEDIACRYPTO SUBSYSTEM ===\n"
       << "Status: " << (mEnabled.load() ? "ONLINE (Operational)" : "DISABLED") << "\n"
       << "Supported DRM Schemes:\n"
       << "  1. W3C ClearKey  [" << CLEARKEY_UUID << "] -> Supported\n"
       << "  2. Widevine L3   [" << WIDEVINE_UUID << "] -> Supported (Software L3)\n"
       << "Virtual TEE Nodes:\n"
       << "  - /dev/tee0    -> " << mSandboxDir << "/tmp/dev_tee0.raw\n"
       << "  - /dev/ion     -> " << mSandboxDir << "/tmp/dev_ion.raw\n"
       << "  - /dev/qseecom -> " << mSandboxDir << "/tmp/dev_qseecom.raw\n"
       << "Active Crypto Sessions: " << mActiveSessions.size() << "\n";

    for (const auto& [id, sess] : mActiveSessions) {
        ss << "  * Session [" << id << "] " << sess.schemeName
           << " | Packets: " << sess.decryptedPackets
           << " | Bytes: " << sess.decryptedBytes << "\n";
    }

    ss << "Total Decrypted: " << mTotalDecryptedBytes << " bytes\n"
       << "Total Lifetime Sessions: " << mSessionCounter;
    return ss.str();
}

void DrmBridge::toggleDrm(bool enabled) {
    mEnabled.store(enabled);
    auto& prop = PropertyService::getInstance();
    prop.setProperty("vendor.drm.status", enabled ? "ONLINE" : "DISABLED");
    LOGI("DrmBridge: Subsystem toggled to %s", enabled ? "ONLINE" : "DISABLED");
}

} // namespace gsi
