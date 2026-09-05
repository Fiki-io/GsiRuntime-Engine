#include "include/sensor_bridge.h"
#include "include/logger.h"
#include "include/property_service.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sstream>
#include <cstring>
#include <iomanip>

namespace gsi {

namespace {

void makeDirsRecursive(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.length(); ++i) {
        current += path[i];
        if (path[i] == '/' || i == path.length() - 1) {
            if (!current.empty() && current != "/") {
                mkdir(current.c_str(), 0777);
            }
        }
    }
}

uint64_t getMonotonicTimeNs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

} // anonymous namespace

SensorBridge& SensorBridge::getInstance() {
    static SensorBridge instance;
    return instance;
}

SensorBridge::~SensorBridge() {
    shutdown();
}

bool SensorBridge::initialize(const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mSensorMutex);

    mSandboxDir = sandboxDir;
    mIioDir = sandboxDir + "/sys/bus/iio/devices/iio:device0";
    mPipePath = sandboxDir + "/tmp/sensor_event";

    makeDirsRecursive(mIioDir);

    // Create sensor FIFO pipe
    unlink(mPipePath.c_str());
    if (mkfifo(mPipePath.c_str(), 0666) != 0) {
        LOGW("SensorBridge: mkfifo failed for %s", mPipePath.c_str());
    }
    mPipeFd = open(mPipePath.c_str(), O_RDWR | O_NONBLOCK);

    mInitialized = true;
    mData = SensorData{}; // Accel: (0, 9.81, 0), Gyro: (0,0,0), Light: 300, Prox: 5.0
    mData.timestampNs = getMonotonicTimeNs();

    syncToSysfs();

    LOGI("SensorBridge: Initialized synthetic IIO sensors at %s (pipe: %s)",
         mIioDir.c_str(), mPipePath.c_str());
    return true;
}

void SensorBridge::shutdown() {
    std::lock_guard<std::mutex> lock(mSensorMutex);
    if (mPipeFd >= 0) {
        close(mPipeFd);
        mPipeFd = -1;
    }
    mInitialized = false;
}

bool SensorBridge::writeSysfsFile(const std::string& path, const std::string& content) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    write(fd, content.data(), content.size());
    close(fd);
    return true;
}

bool SensorBridge::updateSensors(float accelX, float accelY, float accelZ,
                                 float gyroX, float gyroY, float gyroZ,
                                 float lightLux, float proximityCm) {
    std::lock_guard<std::mutex> lock(mSensorMutex);
    if (!mInitialized) return false;

    mData.accelX = accelX;
    mData.accelY = accelY;
    mData.accelZ = accelZ;
    mData.gyroX = gyroX;
    mData.gyroY = gyroY;
    mData.gyroZ = gyroZ;
    mData.lightLux = lightLux >= 0.0f ? lightLux : 0.0f;
    mData.proximityCm = proximityCm >= 0.0f ? proximityCm : 0.0f;
    mData.timestampNs = getMonotonicTimeNs();
    mData.totalEvents++;

    syncToSysfs();

    // Stream telemetry to sensor FIFO pipe if active
    if (mPipeFd >= 0) {
        char packet[256];
        int n = snprintf(packet, sizeof(packet),
                         "SENSOR:%llu,ACCEL:%.3f,%.3f,%.3f,GYRO:%.3f,%.3f,%.3f,LIGHT:%.1f,PROX:%.1f\n",
                         static_cast<unsigned long long>(mData.timestampNs),
                         mData.accelX, mData.accelY, mData.accelZ,
                         mData.gyroX, mData.gyroY, mData.gyroZ,
                         mData.lightLux, mData.proximityCm);
        if (n > 0) {
            write(mPipeFd, packet, static_cast<size_t>(n));
        }
    }

    return true;
}

void SensorBridge::syncToSysfs() {
    if (!mInitialized) return;

    writeSysfsFile(mIioDir + "/name", "virtual-gsi-sensors\n");

    // 1. Accelerometer (m/s^2)
    writeSysfsFile(mIioDir + "/in_accel_x_raw", std::to_string(mData.accelX) + "\n");
    writeSysfsFile(mIioDir + "/in_accel_y_raw", std::to_string(mData.accelY) + "\n");
    writeSysfsFile(mIioDir + "/in_accel_z_raw", std::to_string(mData.accelZ) + "\n");
    writeSysfsFile(mIioDir + "/in_accel_scale", "1.0\n");

    // 2. Gyroscope (rad/s)
    writeSysfsFile(mIioDir + "/in_anglvel_x_raw", std::to_string(mData.gyroX) + "\n");
    writeSysfsFile(mIioDir + "/in_anglvel_y_raw", std::to_string(mData.gyroY) + "\n");
    writeSysfsFile(mIioDir + "/in_anglvel_z_raw", std::to_string(mData.gyroZ) + "\n");
    writeSysfsFile(mIioDir + "/in_anglvel_scale", "1.0\n");

    // 3. Ambient Light (lux)
    writeSysfsFile(mIioDir + "/in_illuminance_input", std::to_string(mData.lightLux) + "\n");

    // 4. Proximity (cm)
    writeSysfsFile(mIioDir + "/in_proximity_input", std::to_string(mData.proximityCm) + "\n");

    // Synchronize with Android System Properties
    std::ostringstream aStr, gStr;
    aStr << std::fixed << std::setprecision(2) << mData.accelX << "," << mData.accelY << "," << mData.accelZ;
    gStr << std::fixed << std::setprecision(2) << mData.gyroX << "," << mData.gyroY << "," << mData.gyroZ;

    PropertyService::getInstance().setProperty("persist.sys.sensor.accel", aStr.str());
    PropertyService::getInstance().setProperty("persist.sys.sensor.gyro", gStr.str());
    PropertyService::getInstance().setProperty("persist.sys.sensor.light", std::to_string(static_cast<int>(mData.lightLux)));
    PropertyService::getInstance().setProperty("persist.sys.sensor.proximity", std::to_string(static_cast<int>(mData.proximityCm)));
}

SensorData SensorBridge::getSensorData() {
    std::lock_guard<std::mutex> lock(mSensorMutex);
    return mData;
}

std::string SensorBridge::getSensorStatsString() {
    std::lock_guard<std::mutex> lock(mSensorMutex);
    std::ostringstream oss;
    oss << "=== VIRTUAL SENSORS & IIO HAL SUBSYSTEM ===\n"
        << std::fixed << std::setprecision(3)
        << "Accelerometer: X=" << mData.accelX << " Y=" << mData.accelY << " Z=" << mData.accelZ << " m/s²\n"
        << "Gyroscope    : X=" << mData.gyroX << " Y=" << mData.gyroY << " Z=" << mData.gyroZ << " rad/s\n"
        << std::setprecision(1)
        << "Ambient Light: " << mData.lightLux << " lux\n"
        << "Proximity    : " << mData.proximityCm << " cm (" << (mData.proximityCm < 3.0f ? "NEAR" : "FAR") << ")\n"
        << "Total Events : " << mData.totalEvents << "\n"
        << "Sysfs Path   : " << mIioDir << "\n"
        << "FIFO Pipe    : " << mPipePath << "\n"
        << "============================================";
    return oss.str();
}

} // namespace gsi
