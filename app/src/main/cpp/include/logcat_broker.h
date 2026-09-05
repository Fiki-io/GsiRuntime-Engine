#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

namespace gsi {

enum class LogBufferId : uint8_t {
    MAIN = 0,
    RADIO = 1,
    EVENTS = 2,
    SYSTEM = 3,
    CRASH = 4,
    STATS = 5,
    SECURITY = 6,
    KERNEL = 7,
    UNKNOWN = 255
};

enum class LogPriority : uint8_t {
    UNKNOWN = 0,
    DEFAULT = 1,
    VERBOSE = 2,
    DEBUG = 3,
    INFO = 4,
    WARN = 5,
    ERROR = 6,
    FATAL = 7,
    SILENT = 8
};

struct LogEntry {
    uint64_t sequence{0};
    LogBufferId bufferId{LogBufferId::MAIN};
    LogPriority priority{LogPriority::INFO};
    pid_t pid{0};
    pid_t tid{0};
    uint64_t sec{0};
    uint64_t nsec{0};
    std::string tag;
    std::string message;

    std::string format() const;
};

struct LogBrokerStats {
    uint64_t totalLogsReceived{0};
    uint64_t totalLogsDropped{0};
    uint64_t totalCrashLogs{0};
    size_t currentBufferSize{0};
    bool isWriterActive{false};
    bool isReaderActive{false};
};

class LogcatBroker {
public:
    static LogcatBroker& getInstance();

    // Starts UNIX domain socket listeners on $SANDBOX/dev/socket/logdw and logdr
    bool start(const std::string& sandboxDir, size_t maxEntries = 5000);
    void stop();
    bool isRunning() const { return mIsRunning.load(); }

    // Direct C++ logging ingestion (called by hooks or native engine)
    void appendLog(LogBufferId bufferId, LogPriority prio, pid_t pid, pid_t tid,
                   const std::string& tag, const std::string& message);

    // Records a native crash tombstone in $SANDBOX/data/tombstones/
    std::string dumpTombstone(pid_t pid, pid_t tid, int signalNum, void* faultAddr,
                              const std::string& reason, const std::string& registerDump,
                              const std::string& backtrace);

    // Read stored logs from ring buffer
    std::string getRecentLogs(int bufferIdFilter = -1, int minPriority = 2, int maxLines = 200) const;
    std::string getLatestTombstone() const;
    void clearLogs();

    LogBrokerStats getStats() const;
    std::string getStatsString() const;

private:
    LogcatBroker();
    ~LogcatBroker();

    LogcatBroker(const LogcatBroker&) = delete;
    LogcatBroker& operator=(const LogcatBroker&) = delete;

    void logdwWorkerLoop();
    void logdrWorkerLoop();
    void handleLogdwPacket(const uint8_t* buffer, size_t length, pid_t clientPid);
    void handleLogdrClient(int clientFd);

    std::string mSandboxDir;
    std::string mLogdwPath; // Writer socket: /dev/socket/logdw
    std::string mLogdrPath; // Reader socket: /dev/socket/logdr
    std::string mTombstoneDir;

    int mLogdwFd{-1};
    int mLogdrFd{-1};

    size_t mMaxEntries{5000};
    uint64_t mSeqCounter{0};
    uint64_t mTotalReceived{0};
    uint64_t mTotalCrashes{0};

    std::atomic<bool> mIsRunning{false};
    std::thread mLogdwThread;
    std::thread mLogdrThread;

    mutable std::mutex mLogMutex;
    std::deque<LogEntry> mRingBuffer;
};

} // namespace gsi
