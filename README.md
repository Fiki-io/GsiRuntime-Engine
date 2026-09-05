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
