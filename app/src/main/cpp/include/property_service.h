#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

namespace gsi {

class PropertyService {
public:
    static PropertyService& getInstance();

    bool start(const std::string& sandboxDir);
    void stop();
    bool isRunning() const { return mIsRunning.load(); }

    void loadFromBuildProp(const std::string& buildPropContent);
    void setProperty(const std::string& key, const std::string& value);
    std::string getProperty(const std::string& key, const std::string& defaultValue = "") const;
    std::string getAllPropertiesFormatted() const;
    size_t getPropertyCount() const;

    const std::string& getSandboxDir() const { return mSandboxDir; }

private:
    PropertyService();
    ~PropertyService();

    PropertyService(const PropertyService&) = delete;
    PropertyService& operator=(const PropertyService&) = delete;

    void workerLoop();
    void handleClient(int clientFd);
    void flushPropertiesToFile();

    std::string mSandboxDir;
    std::string mSocketPath;
    std::string mPropertiesFilePath;
    int mServerFd{-1};

    std::atomic<bool> mIsRunning{false};
    std::thread mWorkerThread;

    mutable std::mutex mPropMutex;
    std::unordered_map<std::string, std::string> mProperties;
};

} // namespace gsi
