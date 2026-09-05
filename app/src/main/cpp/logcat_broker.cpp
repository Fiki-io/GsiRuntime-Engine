#include "include/logcat_broker.h"
#include "include/logger.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace gsi {

namespace {

struct AndroidLogHeader {
    uint8_t id;
    uint16_t tid;
    int32_t sec;
    int32_t nsec;
} __attribute__((packed));

struct LoggerEntryHeader {
    uint16_t len;
    uint16_t hdr_size;
    int32_t pid;
    uint32_t tid;
    uint32_t sec;
    uint32_t nsec;
    uint32_t lid;
    uint32_t uid;
} __attribute__((packed));

const char* priorityToChar(LogPriority prio) {
    switch (prio) {
        case LogPriority::VERBOSE: return "V";
        case LogPriority::DEBUG:   return "D";
        case LogPriority::INFO:    return "I";
        case LogPriority::WARN:    return "W";
        case LogPriority::ERROR:   return "E";
        case LogPriority::FATAL:   return "F";
        case LogPriority::SILENT:  return "S";
        default:                   return "?";
    }
}

const char* bufferIdToString(LogBufferId id) {
    switch (id) {
        case LogBufferId::MAIN:     return "main";
        case LogBufferId::RADIO:    return "radio";
        case LogBufferId::EVENTS:   return "events";
        case LogBufferId::SYSTEM:   return "system";
        case LogBufferId::CRASH:    return "crash";
        case LogBufferId::STATS:    return "stats";
        case LogBufferId::SECURITY: return "security";
        case LogBufferId::KERNEL:   return "kernel";
        default:                    return "unknown";
    }
}

const char* signalToString(int sig) {
    switch (sig) {
        case 11: return "SIGSEGV";
        case 7:  return "SIGBUS";
        case 8:  return "SIGFPE";
        case 4:  return "SIGILL";
        case 6:  return "SIGABRT";
        case 9:  return "SIGKILL";
        case 15: return "SIGTERM";
        default: return "SIGUNKNOWN";
    }
}

} // namespace

std::string LogEntry::format() const {
    std::time_t t = static_cast<std::time_t>(sec);
    std::tm tmVal{};
    localtime_r(&t, &tmVal);

    char timeBuf[32];
    std::snprintf(timeBuf, sizeof(timeBuf), "%02d-%02d %02d:%02d:%02d.%03u",
                  tmVal.tm_mon + 1, tmVal.tm_mday,
                  tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec,
                  static_cast<unsigned int>(nsec / 1000000));

    std::ostringstream ss;
    ss << timeBuf << " "
       << std::setw(5) << pid << " "
       << std::setw(5) << tid << " "
       << priorityToChar(priority) << "/"
       << bufferIdToString(bufferId) << " "
       << std::setw(16) << std::left << tag << ": "
       << message;

    return ss.str();
}

LogcatBroker& LogcatBroker::getInstance() {
    static LogcatBroker instance;
    return instance;
}

LogcatBroker::LogcatBroker() = default;

LogcatBroker::~LogcatBroker() {
    stop();
}

bool LogcatBroker::start(const std::string& sandboxDir, size_t maxEntries) {
    if (mIsRunning.load()) {
        LOGI("LogcatBroker already running");
        return true;
    }

    mSandboxDir = sandboxDir;
    mMaxEntries = maxEntries;

    std::string socketDir = mSandboxDir + "/dev/socket";
    mkdir(socketDir.c_str(), 0755);

    mTombstoneDir = mSandboxDir + "/data/tombstones";
    mkdir(mTombstoneDir.c_str(), 0777);

    mLogdwPath = socketDir + "/logdw";
    mLogdrPath = socketDir + "/logdr";

    unlink(mLogdwPath.c_str());
    unlink(mLogdrPath.c_str());

    // 1. Setup Writer socket (DGRAM)
    mLogdwFd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (mLogdwFd < 0) {
        LOGE("LogcatBroker: Failed to create logdw socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addrDw{};
    addrDw.sun_family = AF_UNIX;
    strncpy(addrDw.sun_path, mLogdwPath.c_str(), sizeof(addrDw.sun_path) - 1);

    if (bind(mLogdwFd, reinterpret_cast<struct sockaddr*>(&addrDw), sizeof(addrDw)) < 0) {
        LOGE("LogcatBroker: Failed to bind logdw: %s", strerror(errno));
        ::close(mLogdwFd);
        mLogdwFd = -1;
        return false;
    }
    chmod(mLogdwPath.c_str(), 0666);

    // 2. Setup Reader socket (STREAM)
    mLogdrFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (mLogdrFd < 0) {
        LOGE("LogcatBroker: Failed to create logdr socket: %s", strerror(errno));
        ::close(mLogdwFd);
        mLogdwFd = -1;
        return false;
    }

    struct sockaddr_un addrDr{};
    addrDr.sun_family = AF_UNIX;
    strncpy(addrDr.sun_path, mLogdrPath.c_str(), sizeof(addrDr.sun_path) - 1);

    if (bind(mLogdrFd, reinterpret_cast<struct sockaddr*>(&addrDr), sizeof(addrDr)) < 0) {
        LOGE("LogcatBroker: Failed to bind logdr: %s", strerror(errno));
        ::close(mLogdwFd);
        mLogdwFd = -1;
        ::close(mLogdrFd);
        mLogdrFd = -1;
        return false;
    }

    if (listen(mLogdrFd, 16) < 0) {
        LOGE("LogcatBroker: Failed to listen on logdr: %s", strerror(errno));
        ::close(mLogdwFd);
        mLogdwFd = -1;
        ::close(mLogdrFd);
        mLogdrFd = -1;
        return false;
    }
    chmod(mLogdrPath.c_str(), 0666);

    mIsRunning.store(true);

    // Insert startup banner entry
    appendLog(LogBufferId::SYSTEM, LogPriority::INFO, getpid(), getpid(),
              "GsiLogcatBroker", "User-Space Logcat Broker initialized successfully (Buffer: 5000 entries)");

    mLogdwThread = std::thread(&LogcatBroker::logdwWorkerLoop, this);
    mLogdrThread = std::thread(&LogcatBroker::logdrWorkerLoop, this);

    LOGI("LogcatBroker: Active on %s and %s", mLogdwPath.c_str(), mLogdrPath.c_str());
    return true;
}

void LogcatBroker::stop() {
    if (!mIsRunning.load()) return;

    LOGI("LogcatBroker: Stopping daemon");
    mIsRunning.store(false);

    if (mLogdwFd >= 0) {
        ::close(mLogdwFd);
        mLogdwFd = -1;
    }
    if (mLogdrFd >= 0) {
        ::close(mLogdrFd);
        mLogdrFd = -1;
    }

    if (!mLogdwPath.empty()) unlink(mLogdwPath.c_str());
    if (!mLogdrPath.empty()) unlink(mLogdrPath.c_str());

    if (mLogdwThread.joinable()) mLogdwThread.join();
    if (mLogdrThread.joinable()) mLogdrThread.join();

    LOGI("LogcatBroker: Stopped");
}

void LogcatBroker::appendLog(LogBufferId bufferId, LogPriority prio, pid_t pid, pid_t tid,
                             const std::string& tag, const std::string& message) {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);

    LogEntry entry;
    entry.bufferId = bufferId;
    entry.priority = prio;
    entry.pid = pid;
    entry.tid = tid;
    entry.sec = ts.tv_sec;
    entry.nsec = ts.tv_nsec;
    entry.tag = tag;
    entry.message = message;

    {
        std::lock_guard<std::mutex> lock(mLogMutex);
        entry.sequence = ++mSeqCounter;
        mTotalReceived++;

        if (bufferId == LogBufferId::CRASH) {
            mTotalCrashes++;
        }

        mRingBuffer.push_back(entry);
        if (mRingBuffer.size() > mMaxEntries) {
            mRingBuffer.pop_front();
        }
    }
}

void LogcatBroker::logdwWorkerLoop() {
    LOGI("LogcatBroker: logdw worker thread started");

    uint8_t buffer[4096];
    while (mIsRunning.load()) {
        struct pollfd pfd{};
        pfd.fd = mLogdwFd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_un senderAddr{};
            socklen_t senderLen = sizeof(senderAddr);
            ssize_t bytes = recvfrom(mLogdwFd, buffer, sizeof(buffer), 0,
                                     reinterpret_cast<struct sockaddr*>(&senderAddr), &senderLen);
            if (bytes > 0) {
                handleLogdwPacket(buffer, static_cast<size_t>(bytes), 0);
            }
        }
    }

    LOGI("LogcatBroker: logdw worker thread exited");
}

void LogcatBroker::handleLogdwPacket(const uint8_t* buffer, size_t length, pid_t clientPid) {
    if (length < sizeof(AndroidLogHeader)) {
        // Fallback: raw text packet
        std::string text(reinterpret_cast<const char*>(buffer), length);
        appendLog(LogBufferId::MAIN, LogPriority::INFO, clientPid, 0, "GsiGuest", text);
        return;
    }

    // Try parsing standard Android log packet
    const auto* hdr = reinterpret_cast<const AndroidLogHeader*>(buffer);
    LogBufferId bufId = (hdr->id <= 7) ? static_cast<LogBufferId>(hdr->id) : LogBufferId::MAIN;
    pid_t tid = static_cast<pid_t>(hdr->tid);

    size_t payloadOffset = sizeof(AndroidLogHeader);
    if (payloadOffset >= length) return;

    // Next byte is priority
    uint8_t rawPrio = buffer[payloadOffset];
    payloadOffset++;
    LogPriority prio = (rawPrio >= 2 && rawPrio <= 7) ? static_cast<LogPriority>(rawPrio) : LogPriority::INFO;

    // Read null-terminated tag
    std::string tag;
    while (payloadOffset < length && buffer[payloadOffset] != '\0') {
        tag += static_cast<char>(buffer[payloadOffset]);
        payloadOffset++;
    }
    if (payloadOffset < length && buffer[payloadOffset] == '\0') {
        payloadOffset++; // skip null terminator
    }
    if (tag.empty()) tag = "GsiGuest";

    // Read remaining message
    std::string msg;
    if (payloadOffset < length) {
        msg = std::string(reinterpret_cast<const char*>(buffer + payloadOffset), length - payloadOffset);
        // Strip trailing newline if present
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
            msg.pop_back();
        }
    }

    appendLog(bufId, prio, clientPid ? clientPid : getpid(), tid, tag, msg);
}

void LogcatBroker::logdrWorkerLoop() {
    LOGI("LogcatBroker: logdr worker thread started");

    while (mIsRunning.load()) {
        struct pollfd pfd{};
        pfd.fd = mLogdrFd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_un clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(mLogdrFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
            if (clientFd >= 0) {
                handleLogdrClient(clientFd);
            }
        }
    }

    LOGI("LogcatBroker: logdr worker thread exited");
}

void LogcatBroker::handleLogdrClient(int clientFd) {
    // Read optional command/filter from logcat
    char reqBuf[256];
    read(clientFd, reqBuf, sizeof(reqBuf) - 1);

    // Snapshot logs from ring buffer
    std::vector<LogEntry> logsSnapshot;
    {
        std::lock_guard<std::mutex> lock(mLogMutex);
        logsSnapshot.assign(mRingBuffer.begin(), mRingBuffer.end());
    }

    // Send formatted logs to client
    for (const auto& entry : logsSnapshot) {
        std::string line = entry.format() + "\n";
        ssize_t sent = send(clientFd, line.c_str(), line.length(), MSG_NOSIGNAL);
        if (sent <= 0) break;
    }

    ::close(clientFd);
}

std::string LogcatBroker::dumpTombstone(pid_t pid, pid_t tid, int signalNum, void* faultAddr,
                                        const std::string& reason, const std::string& registerDump,
                                        const std::string& backtrace) {
    std::lock_guard<std::mutex> lock(mLogMutex);

    // Determine next tombstone filename (e.g. tombstone_00 .. tombstone_09)
    int maxIndex = 0;
    DIR* dir = opendir(mTombstoneDir.c_str());
    if (dir) {
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (std::strncmp(ent->d_name, "tombstone_", 10) == 0) {
                int idx = std::atoi(ent->d_name + 10);
                if (idx >= maxIndex) maxIndex = idx + 1;
            }
        }
        closedir(dir);
    }

    char filename[64];
    std::snprintf(filename, sizeof(filename), "tombstone_%02d", maxIndex % 20);
    std::string fullPath = mTombstoneDir + "/" + filename;

    std::time_t now = std::time(nullptr);
    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&now));

    std::ostringstream ss;
    ss << "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n";
    ss << "Build fingerprint: 'Android/gsi_runtime/generic:14/UP1A.231005.007:userdebug/test-keys'\n";
    ss << "Revision: '0'\n";
    ss << "ABI: 'arm64'\n";
    ss << "Timestamp: " << timeStr << "\n";
    ss << "Process: guest_process (pid: " << pid << ", tid: " << tid << ")\n";
    ss << "Signal: " << signalToString(signalNum) << " (" << signalNum << "), fault addr: " << faultAddr << "\n";
    if (!reason.empty()) {
        ss << "Cause: " << reason << "\n";
    }
    ss << "\n";

    if (!registerDump.empty()) {
        ss << "Registers:\n" << registerDump << "\n\n";
    }

    if (!backtrace.empty()) {
        ss << "Backtrace:\n" << backtrace << "\n\n";
    }

    ss << "Crash Logcat Tail:\n";
    size_t startIdx = (mRingBuffer.size() > 15) ? mRingBuffer.size() - 15 : 0;
    for (size_t i = startIdx; i < mRingBuffer.size(); ++i) {
        ss << "  " << mRingBuffer[i].format() << "\n";
    }

    std::string report = ss.str();

    std::ofstream ofs(fullPath, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << report;
        ofs.close();
        chmod(fullPath.c_str(), 0644);
        LOGI("LogcatBroker: Tombstone saved to %s", fullPath.c_str());
    } else {
        LOGE("LogcatBroker: Failed to write tombstone to %s", fullPath.c_str());
    }

    // Also inject a fatal crash entry into ring buffer
    LogEntry crashEntry;
    crashEntry.sequence = ++mSeqCounter;
    crashEntry.bufferId = LogBufferId::CRASH;
    crashEntry.priority = LogPriority::FATAL;
    crashEntry.pid = pid;
    crashEntry.tid = tid;
    crashEntry.sec = static_cast<uint64_t>(now);
    crashEntry.nsec = 0;
    crashEntry.tag = "DEBUGGERD";
    crashEntry.message = "Fatal signal " + std::to_string(signalNum) + " (" + signalToString(signalNum) + ") at " + fullPath;
    mRingBuffer.push_back(crashEntry);
    mTotalCrashes++;

    return report;
}

std::string LogcatBroker::getRecentLogs(int bufferIdFilter, int minPriority, int maxLines) const {
    std::lock_guard<std::mutex> lock(mLogMutex);

    std::vector<const LogEntry*> matched;
    matched.reserve(std::min(mRingBuffer.size(), static_cast<size_t>(maxLines)));

    for (auto it = mRingBuffer.rbegin(); it != mRingBuffer.rend() && matched.size() < static_cast<size_t>(maxLines); ++it) {
        if (bufferIdFilter >= 0 && static_cast<int>(it->bufferId) != bufferIdFilter) {
            continue;
        }
        if (static_cast<int>(it->priority) < minPriority) {
            continue;
        }
        matched.push_back(&(*it));
    }

    std::ostringstream ss;
    // Print in chronological order
    for (auto it = matched.rbegin(); it != matched.rend(); ++it) {
        ss << (*it)->format() << "\n";
    }

    return ss.str();
}

std::string LogcatBroker::getLatestTombstone() const {
    std::string latestPath;
    time_t latestMtime = 0;

    DIR* dir = opendir(mTombstoneDir.c_str());
    if (dir) {
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (std::strncmp(ent->d_name, "tombstone_", 10) == 0) {
                std::string p = mTombstoneDir + "/" + ent->d_name;
                struct stat st{};
                if (stat(p.c_str(), &st) == 0 && st.st_mtime > latestMtime) {
                    latestMtime = st.st_mtime;
                    latestPath = p;
                }
            }
        }
        closedir(dir);
    }

    if (latestPath.empty()) {
        return "No crash tombstones found in " + mTombstoneDir;
    }

    std::ifstream ifs(latestPath);
    if (!ifs.is_open()) {
        return "Failed to open tombstone at " + latestPath;
    }

    std::ostringstream ss;
    ss << "// Tombstone: " << latestPath << "\n";
    ss << ifs.rdbuf();
    return ss.str();
}

void LogcatBroker::clearLogs() {
    std::lock_guard<std::mutex> lock(mLogMutex);
    mRingBuffer.clear();
}

LogBrokerStats LogcatBroker::getStats() const {
    std::lock_guard<std::mutex> lock(mLogMutex);
    LogBrokerStats s;
    s.totalLogsReceived = mTotalReceived;
    s.totalCrashLogs = mTotalCrashes;
    s.currentBufferSize = mRingBuffer.size();
    s.isWriterActive = (mLogdwFd >= 0);
    s.isReaderActive = (mLogdrFd >= 0);
    return s;
}

std::string LogcatBroker::getStatsString() const {
    std::lock_guard<std::mutex> lock(mLogMutex);
    std::ostringstream ss;
    ss << "Logcat Broker: " << (mIsRunning.load() ? "ACTIVE" : "STOPPED")
       << " | Logs: " << mRingBuffer.size() << "/" << mMaxEntries
       << " | Total Received: " << mTotalReceived
       << " | Crashes: " << mTotalCrashes;
    return ss.str();
}

} // namespace gsi
