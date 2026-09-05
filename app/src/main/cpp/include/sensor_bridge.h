#pragma once

#include <string>
#include <mutex>
#include <cstdint>

namespace gsi {

struct SensorData {
    float accelX = 0.0f;        // m/s^2 (default: Earth gravity downwards)
    float accelY = 9.81f;       // m/s^2
    float accelZ = 0.0f;        // m/s^2
    float gyroX = 0.0f;         // rad/s
    float gyroY = 0.0f;         // rad/s
    float gyroZ = 0.0f;         // rad/s
    float lightLux = 300.0f;    // lux
    float proximityCm = 5.0f;   // cm
    uint64_t timestampNs = 0;   // nanoseconds
    uint64_t totalEvents = 0;
};

class SensorBridge {
public:
    static SensorBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool updateSensors(float accelX, float accelY, float accelZ,
                       float gyroX, float gyroY, float gyroZ,
                       float lightLux, float proximityCm);

    void syncToSysfs();
    std::string getSensorStatsString();
    SensorData getSensorData();

private:
    SensorBridge() = default;
    ~SensorBridge();

    bool writeSysfsFile(const std::string& path, const std::string& content);

    std::mutex mSensorMutex;
    std::string mSandboxDir;
    std::string mIioDir;
    std::string mPipePath;
    int mPipeFd = -1;
    bool mInitialized = false;

    SensorData mData;
};

} // namespace gsi
