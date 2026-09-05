#include "include/battery_bridge.h"
#include "include/logger.h"
#include "include/property_service.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
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

} // anonymous namespace

BatteryBridge& BatteryBridge::getInstance() {
    static BatteryBridge instance;
    return instance;
}

BatteryBridge::~BatteryBridge() {
    shutdown();
}

bool BatteryBridge::initialize(const std::string& sandboxDir) {
    std::lock_guard<std::mutex> lock(mBatteryMutex);

    mSandboxDir = sandboxDir;
    mSysfsBatteryDir = sandboxDir + "/sys/class/power_supply/battery";
    mSysfsAcDir = sandboxDir + "/sys/class/power_supply/ac";
    mSysfsUsbDir = sandboxDir + "/sys/class/power_supply/usb";

    makeDirsRecursive(mSysfsBatteryDir);
    makeDirsRecursive(mSysfsAcDir);
    makeDirsRecursive(mSysfsUsbDir);

    mInitialized = true;
    mInfo = BatteryInfo{}; // default 100%, Charging, Good, 4200mV, 30.0C

    // Sync initial state to disk
    syncToSysfs();

    LOGI("BatteryBridge: Initialized sysfs power_supply at %s", mSysfsBatteryDir.c_str());
    return true;
}

void BatteryBridge::shutdown() {
    std::lock_guard<std::mutex> lock(mBatteryMutex);
    mInitialized = false;
}

bool BatteryBridge::writeSysfsFile(const std::string& path, const std::string& content) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        LOGW("BatteryBridge: Failed to write sysfs file: %s (errno=%d)", path.c_str(), errno);
        return false;
    }
    write(fd, content.data(), content.size());
    close(fd);
    return true;
}

bool BatteryBridge::updateBattery(int capacity,
                                  const std::string& status,
                                  const std::string& health,
                                  int voltageMv,
                                  int tempTenthsC,
                                  bool isPluggedAc,
                                  bool isPluggedUsb) {
    std::lock_guard<std::mutex> lock(mBatteryMutex);
    if (!mInitialized) return false;

    mInfo.capacity = (capacity < 0) ? 0 : (capacity > 100 ? 100 : capacity);
    if (!status.empty()) mInfo.status = status;
    if (!health.empty()) mInfo.health = health;
    if (voltageMv > 0) mInfo.voltageMv = voltageMv;
    if (tempTenthsC > 0) mInfo.tempTenthsC = tempTenthsC;
    mInfo.isPluggedAc = isPluggedAc;
    mInfo.isPluggedUsb = isPluggedUsb;

    syncToSysfs();

    LOGI("BatteryBridge: Updated -> %d%%, Status: %s, Health: %s, %dmV, %d.%d°C, AC:%d USB:%d",
         mInfo.capacity, mInfo.status.c_str(), mInfo.health.c_str(),
         mInfo.voltageMv, mInfo.tempTenthsC / 10, mInfo.tempTenthsC % 10,
         mInfo.isPluggedAc, mInfo.isPluggedUsb);

    return true;
}

void BatteryBridge::syncToSysfs() {
    if (!mInitialized) return;

    // 1. Battery nodes
    writeSysfsFile(mSysfsBatteryDir + "/capacity", std::to_string(mInfo.capacity) + "\n");
    writeSysfsFile(mSysfsBatteryDir + "/status", mInfo.status + "\n");
    writeSysfsFile(mSysfsBatteryDir + "/health", mInfo.health + "\n");
    writeSysfsFile(mSysfsBatteryDir + "/present", (mInfo.present ? "1" : "0") + std::string("\n"));
    writeSysfsFile(mSysfsBatteryDir + "/technology", mInfo.technology + "\n");
    writeSysfsFile(mSysfsBatteryDir + "/voltage_now", std::to_string(mInfo.voltageMv * 1000) + "\n"); // uV
    writeSysfsFile(mSysfsBatteryDir + "/current_now", std::to_string(mInfo.currentMa * 1000) + "\n"); // uA
    writeSysfsFile(mSysfsBatteryDir + "/temp", std::to_string(mInfo.tempTenthsC) + "\n");

    // Full uevent file for Android healthd / BatteryMonitor
    std::ostringstream ueventOss;
    ueventOss << "POWER_SUPPLY_NAME=battery\n"
              << "POWER_SUPPLY_STATUS=" << mInfo.status << "\n"
              << "POWER_SUPPLY_HEALTH=" << mInfo.health << "\n"
              << "POWER_SUPPLY_PRESENT=" << (mInfo.present ? "1" : "0") << "\n"
              << "POWER_SUPPLY_TECHNOLOGY=" << mInfo.technology << "\n"
              << "POWER_SUPPLY_CAPACITY=" << mInfo.capacity << "\n"
              << "POWER_SUPPLY_VOLTAGE_NOW=" << (mInfo.voltageMv * 1000) << "\n"
              << "POWER_SUPPLY_CURRENT_NOW=" << (mInfo.currentMa * 1000) << "\n"
              << "POWER_SUPPLY_TEMP=" << mInfo.tempTenthsC << "\n";
    writeSysfsFile(mSysfsBatteryDir + "/uevent", ueventOss.str());

    // 2. AC Charger nodes
    writeSysfsFile(mSysfsAcDir + "/online", (mInfo.isPluggedAc ? "1\n" : "0\n"));
    writeSysfsFile(mSysfsAcDir + "/type", "Mains\n");
    std::ostringstream acUevent;
    acUevent << "POWER_SUPPLY_NAME=ac\n"
             << "POWER_SUPPLY_TYPE=Mains\n"
             << "POWER_SUPPLY_ONLINE=" << (mInfo.isPluggedAc ? "1\n" : "0\n");
    writeSysfsFile(mSysfsAcDir + "/uevent", acUevent.str());

    // 3. USB Charger nodes
    writeSysfsFile(mSysfsUsbDir + "/online", (mInfo.isPluggedUsb ? "1\n" : "0\n"));
    writeSysfsFile(mSysfsUsbDir + "/type", "USB\n");
    std::ostringstream usbUevent;
    usbUevent << "POWER_SUPPLY_NAME=usb\n"
              << "POWER_SUPPLY_TYPE=USB\n"
              << "POWER_SUPPLY_ONLINE=" << (mInfo.isPluggedUsb ? "1\n" : "0\n");
    writeSysfsFile(mSysfsUsbDir + "/uevent", usbUevent.str());

    // 4. Synchronize with Android System Properties
    PropertyService::getInstance().setProperty("persist.sys.battery.level", std::to_string(mInfo.capacity));
    PropertyService::getInstance().setProperty("persist.sys.battery.status", mInfo.status);
    PropertyService::getInstance().setProperty("hw.ac", mInfo.isPluggedAc ? "1" : "0");
}

BatteryInfo BatteryBridge::getBatteryInfo() {
    std::lock_guard<std::mutex> lock(mBatteryMutex);
    return mInfo;
}

std::string BatteryBridge::getBatteryStatsString() {
    std::lock_guard<std::mutex> lock(mBatteryMutex);
    std::ostringstream oss;
    oss << "=== VIRTUAL BATTERY & POWER SUPPLY SUBSYSTEM ===\n"
        << "Capacity     : " << mInfo.capacity << "%\n"
        << "Status       : " << mInfo.status << "\n"
        << "Health       : " << mInfo.health << "\n"
        << "Voltage      : " << mInfo.voltageMv << " mV (" << (mInfo.voltageMv / 1000.0f) << " V)\n"
        << "Temperature  : " << (mInfo.tempTenthsC / 10.0f) << " °C\n"
        << "Technology   : " << mInfo.technology << "\n"
        << "Charger AC   : " << (mInfo.isPluggedAc ? "CONNECTED" : "DISCONNECTED") << "\n"
        << "Charger USB  : " << (mInfo.isPluggedUsb ? "CONNECTED" : "DISCONNECTED") << "\n"
        << "Sysfs Path   : " << mSysfsBatteryDir << "\n"
        << "=================================================";
    return oss.str();
}

} // namespace gsi
