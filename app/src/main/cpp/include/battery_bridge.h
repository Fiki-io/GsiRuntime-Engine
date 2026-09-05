#pragma once

#include <string>
#include <mutex>
#include <cstdint>

namespace gsi {

struct BatteryInfo {
    int capacity = 100;                 // 0 - 100%
    std::string status = "Charging";    // "Charging", "Discharging", "Full", "Not charging"
    std::string health = "Good";        // "Good", "Overheat", "Dead", "Over voltage", "Cold"
    bool present = true;
    std::string technology = "Li-poly";
    int voltageMv = 4200;               // in mV (sysfs voltage_now will be in uV)
    int currentMa = 500;                // in mA (sysfs current_now will be in uA)
    int tempTenthsC = 300;              // 300 = 30.0 °C
    bool isPluggedAc = true;
    bool isPluggedUsb = false;
};

class BatteryBridge {
public:
    static BatteryBridge& getInstance();

    bool initialize(const std::string& sandboxDir);
    void shutdown();

    bool updateBattery(int capacity,
                       const std::string& status,
                       const std::string& health,
                       int voltageMv,
                       int tempTenthsC,
                       bool isPluggedAc,
                       bool isPluggedUsb);

    void syncToSysfs();
    std::string getBatteryStatsString();
    BatteryInfo getBatteryInfo();

private:
    BatteryBridge() = default;
    ~BatteryBridge();

    bool writeSysfsFile(const std::string& path, const std::string& content);

    std::mutex mBatteryMutex;
    std::string mSandboxDir;
    std::string mSysfsBatteryDir;
    std::string mSysfsAcDir;
    std::string mSysfsUsbDir;
    bool mInitialized = false;

    BatteryInfo mInfo;
};

} // namespace gsi
