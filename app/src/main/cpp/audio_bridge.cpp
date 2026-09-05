#include "include/audio_bridge.h"
#include "include/logger.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace gsi {

AudioBridge& AudioBridge::getInstance() {
    static AudioBridge instance;
    return instance;
}

AudioBridge::AudioBridge() = default;

AudioBridge::~AudioBridge() {
    shutdown();
}

bool AudioBridge::initialize(const std::string& sandboxDir, uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(mBufferMutex);

    mSandboxDir = sandboxDir;
    mSampleRate = sampleRate;
    mChannels = (channels == 1 || channels == 2) ? channels : 2;

    mRingBuffer.assign(RING_BUFFER_SIZE, 0);
    mWritePos = 0;
    mReadPos = 0;
    mAvailableSamples = 0;
    mTotalFramesWritten = 0;
    mTotalFramesRead = 0;
    mUnderruns = 0;

    // Create backing file for ALSA redirect
    std::string tmpDir = mSandboxDir + "/tmp";
    mkdir(tmpDir.c_str(), 0777);

    mAudioFifoPath = tmpDir + "/audio_pcm.raw";
    int fd = open(mAudioFifoPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        ::close(fd);
        chmod(mAudioFifoPath.c_str(), 0666);
    }

    mIsRunning.store(true);
    LOGI("AudioBridge: Initialized at %u Hz, %u channels (Buffer: %zu samples)",
         mSampleRate, mChannels, RING_BUFFER_SIZE);
    return true;
}

void AudioBridge::shutdown() {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    mIsRunning.store(false);
    mRingBuffer.clear();
    mRingBuffer.shrink_to_fit();
    mWritePos = 0;
    mReadPos = 0;
    mAvailableSamples = 0;
    LOGI("AudioBridge: Shutdown complete");
}

size_t AudioBridge::writeSamples(const int16_t* samples, size_t count) {
    if (!mIsRunning.load() || !samples || count == 0) return 0;

    std::lock_guard<std::mutex> lock(mBufferMutex);
    size_t spaceLeft = RING_BUFFER_SIZE - mAvailableSamples;
    size_t toWrite = std::min(count, spaceLeft);

    for (size_t i = 0; i < toWrite; ++i) {
        mRingBuffer[mWritePos] = samples[i];
        mWritePos = (mWritePos + 1) % RING_BUFFER_SIZE;
    }

    mAvailableSamples += toWrite;
    mTotalFramesWritten += (toWrite / mChannels);

    return toWrite;
}

size_t AudioBridge::readSamples(int16_t* outSamples, size_t maxCount) {
    if (!mIsRunning.load() || !outSamples || maxCount == 0) return 0;

    std::lock_guard<std::mutex> lock(mBufferMutex);
    size_t toRead = std::min(maxCount, mAvailableSamples);

    for (size_t i = 0; i < toRead; ++i) {
        outSamples[i] = mRingBuffer[mReadPos];
        mReadPos = (mReadPos + 1) % RING_BUFFER_SIZE;
    }

    mAvailableSamples -= toRead;
    mTotalFramesRead += (toRead / mChannels);

    // If caller requested more than available, pad remainder with silence
    if (toRead < maxCount) {
        std::memset(outSamples + toRead, 0, (maxCount - toRead) * sizeof(int16_t));
        if (toRead == 0) {
            mUnderruns++;
        }
    }

    return toRead;
}

bool AudioBridge::synthesizeChime(int type) {
    if (!mIsRunning.load()) return false;

    constexpr double PI = 3.14159265358979323846;
    std::vector<int16_t> generated;

    if (type == 1) {
        // Dual-Chord Android Boot Chime (1.2 seconds, 48000 Hz, Stereo)
        // Chord: C-Maj-7th (C5=523.25, E5=659.25, G5=783.99, B5=987.77) + Higher shimmer
        double duration = 1.2;
        size_t numFrames = static_cast<size_t>(mSampleRate * duration);
        generated.resize(numFrames * 2);

        double freqC5 = 523.25;
        double freqE5 = 659.25;
        double freqG5 = 783.99;
        double freqB5 = 987.77;
        double freqC6 = 1046.50;

        for (size_t i = 0; i < numFrames; ++i) {
            double t = static_cast<double>(i) / mSampleRate;

            // Envelope: 40ms attack, exponential decay (tau = 0.35s)
            double attack = std::min(1.0, t / 0.04);
            double decay = std::exp(-t / 0.35);
            double env = attack * decay;

            // Harmonics synthesis
            double s1 = std::sin(2.0 * PI * freqC5 * t);
            double s2 = std::sin(2.0 * PI * freqE5 * t);
            double s3 = std::sin(2.0 * PI * freqG5 * t);
            double s4 = std::sin(2.0 * PI * freqB5 * t);
            double s5 = std::sin(2.0 * PI * freqC6 * t) * 0.5;

            // Spatial stereo distribution
            double leftSignal  = (s1 * 0.35 + s3 * 0.30 + s5 * 0.20) * env;
            double rightSignal = (s2 * 0.35 + s4 * 0.30 + s5 * 0.20) * env;

            // Shimmer vibrato
            double shimmer = 1.0 + 0.08 * std::sin(2.0 * PI * 6.0 * t);
            leftSignal *= shimmer;
            rightSignal *= shimmer;

            // Clamp and convert to 16-bit signed PCM
            int16_t pcmL = static_cast<int16_t>(std::clamp(leftSignal * 24000.0, -32767.0, 32767.0));
            int16_t pcmR = static_cast<int16_t>(std::clamp(rightSignal * 24000.0, -32767.0, 32767.0));

            generated[i * 2]     = pcmL;
            generated[i * 2 + 1] = pcmR;
        }
    } else {
        // Retro Cyber Ping / Alert Chime (0.45 seconds)
        double duration = 0.45;
        size_t numFrames = static_cast<size_t>(mSampleRate * duration);
        generated.resize(numFrames * 2);

        for (size_t i = 0; i < numFrames; ++i) {
            double t = static_cast<double>(i) / mSampleRate;
            double env = std::exp(-t / 0.12);

            // Frequency sweep: 440 Hz -> 880 Hz
            double freq = 440.0 + 440.0 * (1.0 - std::exp(-t / 0.08));
            double sample = std::sin(2.0 * PI * freq * t) * env;

            int16_t pcm = static_cast<int16_t>(std::clamp(sample * 20000.0, -32767.0, 32767.0));
            generated[i * 2]     = pcm;
            generated[i * 2 + 1] = pcm;
        }
    }

    size_t written = writeSamples(generated.data(), generated.size());
    LOGI("AudioBridge: Synthesized chime (type %d): %zu/%zu samples injected to buffer",
         type, written, generated.size());
    return written > 0;
}

AudioStats AudioBridge::getStats() const {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    AudioStats s;
    s.sampleRate = mSampleRate;
    s.channels = mChannels;
    s.totalFramesWritten = mTotalFramesWritten;
    s.totalFramesRead = mTotalFramesRead;
    s.underruns = mUnderruns;
    s.bufferFillBytes = mAvailableSamples * sizeof(int16_t);
    s.bufferCapacityBytes = RING_BUFFER_SIZE * sizeof(int16_t);
    s.isRunning = mIsRunning.load();
    return s;
}

std::string AudioBridge::getStatsString() const {
    std::lock_guard<std::mutex> lock(mBufferMutex);
    double fillPct = (static_cast<double>(mAvailableSamples) / RING_BUFFER_SIZE) * 100.0;

    std::ostringstream ss;
    ss << "Virtual Audio HAL: " << (mIsRunning.load() ? "ONLINE" : "OFFLINE")
       << " | " << (mSampleRate / 1000) << "kHz Stereo 16-bit"
       << " | Buffer: " << std::fixed << std::setprecision(1) << fillPct << "% ("
       << (mAvailableSamples / mChannels) << "/" << (RING_BUFFER_SIZE / mChannels) << " frames)"
       << " | Out: " << mTotalFramesRead
       << " | In: " << mTotalFramesWritten
       << " | Underruns: " << mUnderruns;
    return ss.str();
}

} // namespace gsi
