#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace gsi {

struct AudioStats {
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    uint64_t totalFramesWritten{0};
    uint64_t totalFramesRead{0};
    uint64_t underruns{0};
    size_t bufferFillBytes{0};
    size_t bufferCapacityBytes{0};
    bool isRunning{false};
};

class AudioBridge {
public:
    static AudioBridge& getInstance();

    bool initialize(const std::string& sandboxDir, uint32_t sampleRate = 48000, uint32_t channels = 2);
    void shutdown();
    bool isRunning() const { return mIsRunning.load(); }

    // Producer API: Write 16-bit PCM samples into the circular buffer
    size_t writeSamples(const int16_t* samples, size_t count);

    // Consumer API: Read 16-bit PCM samples to feed into Android AudioTrack
    size_t readSamples(int16_t* outSamples, size_t maxCount);

    // Synthesizes a boot chime / audio test tone into the ring buffer
    bool synthesizeChime(int type = 1);

    AudioStats getStats() const;
    std::string getStatsString() const;

private:
    AudioBridge();
    ~AudioBridge();

    AudioBridge(const AudioBridge&) = delete;
    AudioBridge& operator=(const AudioBridge&) = delete;

    std::string mSandboxDir;
    std::string mAudioFifoPath;

    uint32_t mSampleRate{48000};
    uint32_t mChannels{2};

    std::atomic<bool> mIsRunning{false};

    // Circular Ring Buffer (Capacity: 65,536 samples = 32,768 frames = 131,072 bytes)
    static constexpr size_t RING_BUFFER_SIZE = 65536;
    std::vector<int16_t> mRingBuffer;

    mutable std::mutex mBufferMutex;
    size_t mWritePos{0};
    size_t mReadPos{0};
    size_t mAvailableSamples{0};

    uint64_t mTotalFramesWritten{0};
    uint64_t mTotalFramesRead{0};
    uint64_t mUnderruns{0};
};

} // namespace gsi
