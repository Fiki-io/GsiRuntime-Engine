#pragma once

#include <cstdint>
#include <string>

namespace gsi {

enum class ImageFormat {
    UNKNOWN = 0,
    ANDROID_SPARSE = 1,
    RAW_EXT4 = 2
};

struct ImageVerificationResult {
    bool isValid = false;
    ImageFormat format = ImageFormat::UNKNOWN;
    uint32_t blockSize = 0;
    uint64_t totalBlocks = 0;
    uint64_t uncompressedSizeBytes = 0;
    std::string description;
    std::string volumeName;
};

class ImageVerifier {
public:
    static ImageVerificationResult verifyFileDescriptor(int fd);
    static ImageVerificationResult verifyFilePath(const std::string& path);
};

} // namespace gsi
