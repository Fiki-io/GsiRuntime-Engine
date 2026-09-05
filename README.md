# GSI Runtime Engine (Zero-Root Untouched Android GSI Runtime)

A native Android runtime application capable of booting and executing an untouched Generic System Image (`system.img`) on an unrooted host Android device without unlocked bootloader, without kernel modifications, and without hardware pKVM / AVF virtualization.

Built with **Kotlin + Modern C++20 NDK**, direct `ANativeWindow` hardware rendering, user-space EXT4/Sparse VFS, in-process syscall hooking via `LD_PRELOAD`, user-space Binder broker, user-space Property Service, Gralloc emulation, Virtual Network/DNS Bridge, and a Real-Time Logcat/Tombstone Broker.

---

## ⚡ Core Architecture

```
+-----------------------------------------------------------------------------------+
|                            GSI RUNTIME ENGINE (HOST APP)                           |
+-----------------------------------------------------------------------------------+
|  [Hardware Display Pipeline]        [User-Space EXT4 & Sparse VFS Engine]         |
|  • ANativeWindow Direct Blit @60FPS • Zero-disk block reader (0xED26FF3A / 0xEF53)|
|  • 3-Mode Pipeline: Boot Splash /   • Inode Table & Extents Tree direct parser    |
|    VFB Guest / Gradient GPU test    • Auto build.prop extraction & inspection     |
+-----------------------------------------------------------------------------------+
|  [Process Spawner & Sandbox VFS]    [In-Process Syscall Hooking (libgsi_hook.so)] |
|  • fork() + setsid() + UNIX pipes   • LD_PRELOAD unshare / mount / chroot bypass  |
|  • Environment isolation            • UID/GID 0 root spoofing & SELinux permissive|
|  • Interactive terminal console     • /dev/graphics/fb0 -> $SANDBOX/tmp/vfb0      |
+-----------------------------------------------------------------------------------+
|  [Virtual Binder Driver Broker]     [User-Space Android Property Service]         |
|  • In-memory IPC protocol v8        • UNIX socket /dev/socket/property_service    |
|  • Binder context manager           • Bionic 128-byte & modern property packets   |
|  • Thread pool & ref counting       • __system_property_get/set interception      |
+-----------------------------------------------------------------------------------+
|  [Init.rc Engine & Supervisor]      [Gralloc Bridge & Synthetic Graphics]         |
|  • /init.rc & /system/etc/init/*.rc • Shared memory GraphicBuffer allocator       |
|  • 4-phase boot (early-init->boot)  • hw_get_module("gralloc" / "hwcomposer") hook|
|  • Service manager & controller     • 60 FPS neon Bugdroid boot splash compositor |
+-----------------------------------------------------------------------------------+
|  [Virtual Network & DNS Bridge]     [Real-Time Logcat & Tombstone Broker]         |
|  • /etc/resolv.conf & /etc/hosts    • /dev/socket/logdw (Bionic writer datagram)  |
|  • /dev/socket/netd daemon listener • /dev/socket/logdr (stream reader for logcat)|
|  • SOCK_RAW -> SOCK_DGRAM ICMP hook • 5,000-entry in-memory ring buffer (main/etc)|
|  • Unprivileged passthrough TCP/UDP • Debuggerd crash dumper (/data/tombstones/)  |
+-----------------------------------------------------------------------------------+
```

---

## 🛠️ Feature Overview

1. **Direct ANativeWindow Hardware Rendering (Tahap 1)**:
   - High-throughput buffer swap pipeline rendering directly to `SurfaceView` at 60 FPS.
   - Foreground service LMK shield preventing memory eviction by the host OS.
2. **User-Space EXT4 & Sparse VFS (Tahap 2)**:
   - Reads raw `system.img` or sparse Android images block-by-block with zero disk extraction.
   - Direct inode and extent traversal to extract and execute guest binaries on-the-fly.
3. **Process Spawner & Isolated Sandbox (Tahap 3)**:
   - Process launcher using POSIX `fork()`, `setsid()`, and non-blocking streaming UNIX pipes.
   - Interactive Cyberpunk terminal with command chips.
4. **Virtual Framebuffer & Multi-Touch Input Bridge (Tahap 4)**:
   - Shared memory framebuffer (`mmap` at `$SANDBOX/tmp/vfb0`) supporting 720x1280 RGBA8888.
   - Android `MotionEvent` to Linux `struct input_event` translation for multi-touch support.
5. **Syscall Hooking & Virtual Binder Broker (Tahap 5)**:
   - Standalone `libgsi_hook.so` loaded via `LD_PRELOAD`.
   - In-memory Virtual Binder Driver emulating Android kernel IPC ioctl protocol v8.
6. **Property Service & Privilege Spoofing Engine (Tahap 6)**:
   - Standalone daemon listening on `$SANDBOX/dev/socket/property_service`.
   - Root spoofing (`getuid()`, `geteuid()` -> 0) and SELinux permissive mode simulation.
7. **Init Boot Sequence & Service Supervisor (Tahap 7)**:
   - Parses `.rc` scripts from the mounted VFS.
   - Boots system through structured triggers (`early-init`, `init`, `boot`, `completed`).
8. **Synthetic Graphics Bridge & Gralloc Emulation (Tahap 8)**:
   - Virtual `gralloc` and `hwcomposer` module interception (`HARDWARE_MODULE_TAG = 0x48574D54`).
   - 60 FPS Android Boot Splash Compositor.
9. **Virtual Network & DNS Bridge (Pilar 1)**:
   - Automatically provisions `$SANDBOX/etc/resolv.conf` and `$SANDBOX/etc/hosts`.
   - UNIX domain socket listener at `$SANDBOX/dev/socket/netd`.
   - ICMP ping socket redirection to unprivileged `SOCK_DGRAM`.
10. **Real-Time Logcat Broker & Tombstone Crash Analyzer (Pilar 2)**:
    - User-space `logd` writer (`/dev/socket/logdw`) and reader (`/dev/socket/logdr`).
    - 5,000-entry ring buffer supporting `main`, `system`, `radio`, `events`, and `crash`.
    - Automated `debuggerd` crash tombstone generation at `$SANDBOX/data/tombstones/`.
11. **Virtual Audio HAL Bridge & PCM AudioTrack Engine (Pilar 3)**:
    - Shared memory audio ring buffer (128 KB) for 48 kHz stereo 16-bit Signed Little Endian PCM.
    - Intercepts `hw_get_module("audio")` (`HARDWARE_MODULE_TAG = 0x48574D54`) preventing `audioserver` crashes.
    - Redirects `/dev/snd/*` kernel nodes to the virtual PCM audio backing file.
    - Native dual-chord cyber boot chime synthesizer and real-time `AudioTrack` playback stream.
12. **Hardware Navigation & Virtual Input Keys (Pilar 4)**:
    - Linux input subsystem keycode emulation (`KEY_BACK=158`, `KEY_HOME=172`, `KEY_RECENTS=580`, `KEY_POWER=116`, `KEY_VOLUMEUP=115`, `KEY_VOLUMEDOWN=114`).
    - Full click cycle injection (`EV_KEY` press -> `EV_SYN` -> `EV_KEY` release -> `EV_SYN`).
    - Syscall interception diverting guest `/dev/input/*` (`EventHub`, `InputReader`) to `$SANDBOX/tmp/touch_event`.
    - Integrated Cyber Navigation Bar UI overlay and terminal chips for instant guest control.
13. **Virtual Battery & Power Management Subsystem (Pilar 5)**:
    - Synthetic Linux sysfs power supply hierarchy (`$SANDBOX/sys/class/power_supply/battery/` and `{ac,usb}/`).
    - Full sysfs node support: `capacity`, `status`, `health`, `present`, `technology`, `voltage_now`, `current_now`, `temp`, and aggregated `uevent` block for Android `healthd`.
    - Real-time automatic synchronization with host Android battery via `GsiBatteryManager` (`ACTION_BATTERY_CHANGED`).
    - In-process syscall redirection of `/sys/class/power_supply/*` in `libgsi_hook.so`.
    - Interactive spoofing chips (`[bat_sync]`, `[bat_100]`, `[bat_50]`, `[bat_15]`, `[bat_plug]`, `[battery_stats]`).
14. **Virtual Sensor Subsystem & Sensors HAL Bridge (Pilar 6)**:
    - Synthetic Linux Industrial I/O (IIO) sysfs hierarchy (`$SANDBOX/sys/bus/iio/devices/iio:device0/`).
    - 4-in-1 sensor telemetry: Accelerometer (m/s²), Gyroscope (rad/s), Ambient Light (lux), and Proximity (cm).
    - Android Sensors HAL module interception via `hw_get_module("sensors")` in `libgsi_hook.so` (`HARDWARE_MODULE_TAG = 0x48574D54`).
    - Real-time host device motion mirroring via `GsiSensorManager` and continuous streaming FIFO pipe (`$SANDBOX/tmp/sensor_event`).
    - Motion spoofing chips (`[sensor_sync]`, `[orient_portrait]`, `[orient_landscape]`, `[shake_device]`, `[prox_near]`, `[sensor_stats]`).
15. **Virtual Camera & Media Codec Stub HAL Subsystem (Pilar 7)**:
    - User-space Virtual Video4Linux2 (V4L2) kernel device emulation: `/dev/video0` (Back Camera, 1280x720 HD) & `/dev/video1` (Front Camera, 640x480 VGA).
    - `hw_get_module("camera")` HAL interception in `libgsi_hook.so` returning valid `camera_module_t` (`0x48574D54`) to satisfy Android `cameraserver` and prevent crash loops.
    - Real-time 30 FPS YUYV/RGB frame generator with animated SMPTE color bars and cyber neon scanline indicators.
    - Direct syscall redirection of `/dev/video*` to sandbox storage nodes (`$SANDBOX/tmp/v4l2_video0.raw`, `$SANDBOX/tmp/v4l2_video1.raw`).
    - Interactive camera stream control chips (`[cam_start]`, `[cam_stop]`, `[cam_switch]`, `[camera_stats]`).
16. **Multi-User / Storage Emulation & FUSE/sdcardfs Isolation Bridge (Pilar 8)**:
    - User-space Virtual Storage Subsystem emulating `/storage/emulated/0` -> `$SANDBOX/data/media/0` and `/sdcard`.
    - Automatic bootstrapping of standard Android media directory hierarchy (`DCIM/Camera`, `Pictures/Screenshots`, `Download`, `Music`, `Movies`, `Documents`, `Android/data`, `Android/obb`).
    - Syscall hooks for `mount()` and `umount2()` faking success to prevent `vold` and init script failures.
    - Filesystem metric spoofing via `statfs`, `statfs64`, `statvfs`, and `statvfs64` reporting 64 GB capacity with ~56 GB free space, completely eliminating Android `LowStorageException` and APK installation denials.
    - Synthetic `/proc/mounts` provider and `/dev/fuse` redirection.
    - Interactive storage management chips (`[storage_stats]`, `[storage_sample]`, `[storage_wipe]`).
17. **Virtual Wi-Fi & Cellular RIL / Telephony Stub Subsystem (Pilar 9)**:
    - User-space Radio Interface Layer (RIL) daemon emulating UNIX domain sockets `/dev/socket/rild` and `/dev/socket/rild-debug`.
    - Virtual Wi-Fi control socket `/dev/socket/wpa_wlan0` emulation for `wpa_supplicant`.
    - Radio and Wi-Fi HAL module interception via `hw_get_module("radio")` and `hw_get_module("wifi")` returning valid HAL descriptor `0x48574D54` to stop `com.android.phone` and `WifiService` crash loops.
    - Full Virtual 5G SIM telemetry (Carrier: "CyberGSI 5G", MCC/MNC: 310-260, Signal: 5/5 bars, IMEI: 860123456789012, Data: CONNECTED).
    - Virtual Wi-Fi networking state (SSID: "GSI-Virtual-WiFi", IP: 192.168.1.150, Speed: 866 Mbps 802.11ac, RSSI: -42 dBm).
    - Interactive hardware toggling chips (`[ril_stats]`, `[sim_toggle]`, `[wifi_toggle]`).
18. **Virtual Bluetooth HAL Subsystem (Pilar 10)**:
    - User-space Virtual Host Controller Interface (`/dev/vhci`) and `/dev/rfkill` emulation.
    - Android Bluetooth HAL interception via `hw_get_module("bluetooth")` returning valid HAL descriptor `0x48574D54` to satisfy Fluoride / Floss stack and `BluetoothService`.
    - Virtual Dual-Mode Bluetooth 5.2 controller (Device Name: "CyberGSI-Bluetooth", BD_ADDR: `00:1A:7D:DA:71:13`, Scan Mode: Connectable & Discoverable).
    - Simulated BLE peripheral discovery engine ("CyberWatch Ultra", "CyberBuds Pro ANC", "GSI Beacon Hub", "CyberKey Fob").
    - System property sync (`persist.sys.bluetooth.*`, `bluetooth.status`, A2DP / HFP profiles).
    - Interactive hardware control chips (`[bt_stats]`, `[bt_toggle]`, `[bt_scan]`).
19. **Real GSI Live Extraction & Execution Test Runner Engine**:
    - Recursive system unpacker (`GsiExtractor`) extracting essential directories from raw untouched GSI `system.img` (`/system/bin/`, `/system/etc/init/`, `/system/lib64/`, `/system/build.prop`, `/init.rc`) directly into the sandbox with proper POSIX permissions (`0755` for executables).
    - Automated GSI Execution Test Suite (`InitRunner`) providing 7-point runtime health check:
      1. Sandbox Filesystem Isolation (`system`, `data`, `proc`, `sys`, `dev`, `tmp`).
      2. LD_PRELOAD Syscall Hook Engine deployment (`libgsi_hook.so`).
      3. Guest Executable Binaries validation (`toybox`, `sh`, `toolbox`).
      4. Android Property Service Daemon IPC & socket verification.
      5. User-Space EXT4 / Sparse VFS superblock & inode integrity.
      6. Process Spawner & Root/SELinux privilege spoofing execution.
      7. Android Init Supervisor & registered service states.
    - One-click interactive execution buttons & quick chips: `[extract_gsi]`, `[run_gsi_tests]`, `[spawn_sh]`.
20. **Virtual DRM & MediaCrypto Subsystem (Pilar 11)**:
    - User-space DRM scheme support: W3C ClearKey (`e660e10f-26ac-4428-9b77-98f653650536`) and Widevine Modular L3 (`edef8ba9-79d6-4ace-a3c8-27dcd51d21ed`).
    - Mock TEE & cryptographic device nodes (`/dev/tee0`, `/dev/ion`, `/dev/qseecom`) redirected to user sandbox backing stores.
    - Hardware module interception for `hw_get_module("drm")` and `hw_get_module("crypto")` returning valid HAL descriptor `0x48574D54` to prevent `mediadrmserver` and media framework crash loops.
    - Software AES-128 cryptographic sample decryption pipeline for protected media frames.
    - System property synchronization (`drm.service.enabled`, `media.mediadrmservice.enable`, `ro.hardware.drm`, `vendor.drm.widevine.security_level = L3`).
    - Interactive diagnostic chips: `[drm_stats]`, `[drm_test]`, `[drm_toggle]`.

---

## 🏗️ Build & Requirements

- **Android SDK**: Compile SDK 35, Min SDK 28, Target SDK 34
- **NDK**: 27.3.13750724 (C++20 standard)
- **ABIs**: `arm64-v8a`, `x86_64`
- **Gradle**: 8.9 with Android Gradle Plugin 8.5.2

### Build from Command Line
```bash
./gradlew assembleDebug
```
Output APK is located at: `app/build/outputs/apk/debug/app-debug.apk`

---

## 📄 License
GPL-3.0 License.
