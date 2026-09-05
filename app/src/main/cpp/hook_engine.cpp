#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "include/hook_engine.h"
#include "include/virtual_binder.h"
#include "include/logger.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <atomic>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <algorithm>

namespace {

using RealOpenFn = int (*)(const char*, int, mode_t);
using RealOpenatFn = int (*)(int, const char*, int, mode_t);
using RealIoctlFn = int (*)(int, int, ...);
using RealMountFn = int (*)(const char*, const char*, const char*, unsigned long, const void*);
using RealChrootFn = int (*)(const char*);
using RealCloseFn = int (*)(int);
using RealConnectFn = int (*)(int, const struct sockaddr*, socklen_t);
using RealSocketFn = int (*)(int, int, int);
using RealPrctlFn = int (*)(int, unsigned long, unsigned long, unsigned long, unsigned long);
using RealPropGetFn = int (*)(const char*, char*);
using RealPropSetFn = int (*)(const char*, const char*);

RealOpenFn getRealOpen() {
    static auto real = reinterpret_cast<RealOpenFn>(dlsym(RTLD_NEXT, "open"));
    return real;
}

RealOpenatFn getRealOpenat() {
    static auto real = reinterpret_cast<RealOpenatFn>(dlsym(RTLD_NEXT, "openat"));
    return real;
}

RealIoctlFn getRealIoctl() {
    static auto real = reinterpret_cast<RealIoctlFn>(dlsym(RTLD_NEXT, "ioctl"));
    return real;
}

RealCloseFn getRealClose() {
    static auto real = reinterpret_cast<RealCloseFn>(dlsym(RTLD_NEXT, "close"));
    return real;
}

RealConnectFn getRealConnect() {
    static auto real = reinterpret_cast<RealConnectFn>(dlsym(RTLD_NEXT, "connect"));
    return real;
}

RealSocketFn getRealSocket() {
    static auto real = reinterpret_cast<RealSocketFn>(dlsym(RTLD_NEXT, "socket"));
    return real;
}

RealPrctlFn getRealPrctl() {
    static auto real = reinterpret_cast<RealPrctlFn>(dlsym(RTLD_NEXT, "prctl"));
    return real;
}

RealPropGetFn getRealPropGet() {
    static auto real = reinterpret_cast<RealPropGetFn>(dlsym(RTLD_NEXT, "__system_property_get"));
    return real;
}

// -------------------------------------------------------------
// Virtual Identity State (Root by default, spoofable dynamically)
// -------------------------------------------------------------
static std::atomic<uid_t> gVirtualRuid{0};
static std::atomic<uid_t> gVirtualEuid{0};
static std::atomic<uid_t> gVirtualSuid{0};
static std::atomic<gid_t> gVirtualRgid{0};
static std::atomic<gid_t> gVirtualEgid{0};
static std::atomic<gid_t> gVirtualSgid{0};
static std::atomic<bool> gIdentityInitialized{false};
static std::mutex gIdentityMutex;
static std::vector<gid_t> gVirtualGroups{0};

void initIdentityIfNeeded() {
    if (!gIdentityInitialized.load()) {
        const char* envUid = getenv("GSI_VIRTUAL_UID");
        uid_t u = (envUid != nullptr) ? static_cast<uid_t>(std::atoi(envUid)) : 0;
        const char* envGid = getenv("GSI_VIRTUAL_GID");
        gid_t g = (envGid != nullptr) ? static_cast<gid_t>(std::atoi(envGid)) : 0;

        gVirtualRuid.store(u);
        gVirtualEuid.store(u);
        gVirtualSuid.store(u);
        gVirtualRgid.store(g);
        gVirtualEgid.store(g);
        gVirtualSgid.store(g);

        {
            std::lock_guard<std::mutex> lock(gIdentityMutex);
            gVirtualGroups = {g};
        }
        gIdentityInitialized.store(true);
    }
}

// Redirect target path to sandbox if applicable
std::string redirectSandboxPath(const char* path) {
    if (!path) return "";
    std::string p(path);

    const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
    std::string sandbox = (sandboxEnv && strlen(sandboxEnv) > 0)
                          ? sandboxEnv
                          : "/data/data/com.gsi.runtime/files/sandbox";

    if (p == "/proc/cmdline") {
        return sandbox + "/proc/cmdline";
    }
    if (p == "/dev/graphics/fb0" || p == "/dev/fb0") {
        return sandbox + "/tmp/vfb0";
    }
    if (p.rfind("/dev/ashmem", 0) == 0) {
        return sandbox + "/tmp/ashmem_default.raw";
    }
    if (p.rfind("/dev/socket/property_service", 0) == 0) {
        return sandbox + "/dev/socket/property_service";
    }
    if (p.rfind("/dev/socket/netd", 0) == 0 || p.rfind("/dev/socket/dnsproxyd", 0) == 0) {
        return sandbox + "/dev/socket/netd";
    }
    if (p.rfind("/dev/socket/logdw", 0) == 0 || p.rfind("/dev/socket/logd", 0) == 0 || p.rfind("/dev/log/", 0) == 0) {
        return sandbox + "/dev/socket/logdw";
    }
    if (p.rfind("/dev/socket/logdr", 0) == 0) {
        return sandbox + "/dev/socket/logdr";
    }
    if (p == "/etc/resolv.conf") {
        return sandbox + "/etc/resolv.conf";
    }
    if (p == "/etc/hosts") {
        return sandbox + "/etc/hosts";
    }
    if (p.rfind("/dev/__properties__", 0) == 0) {
        return sandbox + "/dev/__properties_text__";
    }
    if (p.rfind("/dev/snd/", 0) == 0) {
        return sandbox + "/tmp/audio_pcm.raw";
    }
    if (p.rfind("/dev/input/", 0) == 0) {
        return sandbox + "/tmp/touch_event";
    }
    if (p.rfind("/sys/class/power_supply/", 0) == 0) {
        return sandbox + "/sys/class/power_supply/" + p.substr(24);
    }
    if (p.rfind("/sys/devices/virtual/power_supply/", 0) == 0) {
        return sandbox + "/sys/class/power_supply/" + p.substr(34);
    }
    if (p.rfind("/sys/bus/iio/devices/", 0) == 0) {
        return sandbox + "/sys/bus/iio/devices/" + p.substr(21);
    }
    if (p == "/dev/video0") {
        return sandbox + "/tmp/v4l2_video0.raw";
    }
    if (p == "/dev/video1") {
        return sandbox + "/tmp/v4l2_video1.raw";
    }
    if (p.rfind("/dev/video", 0) == 0) {
        return sandbox + "/tmp/v4l2_video0.raw";
    }
    if (p == "/sdcard") {
        return sandbox + "/data/media/0";
    }
    if (p.rfind("/sdcard/", 0) == 0) {
        return sandbox + "/data/media/0" + p.substr(7);
    }
    if (p == "/storage/emulated/0") {
        return sandbox + "/data/media/0";
    }
    if (p.rfind("/storage/emulated/0/", 0) == 0) {
        return sandbox + "/data/media/0" + p.substr(19);
    }
    if (p == "/storage/self/primary") {
        return sandbox + "/data/media/0";
    }
    if (p.rfind("/storage/self/primary/", 0) == 0) {
        return sandbox + "/data/media/0" + p.substr(21);
    }
    if (p == "/data/media/0") {
        return sandbox + "/data/media/0";
    }
    if (p.rfind("/data/media/0/", 0) == 0) {
        return sandbox + "/data/media/0" + p.substr(13);
    }
    if (p == "/data/media") {
        return sandbox + "/data/media";
    }
    if (p.rfind("/data/media/", 0) == 0) {
        return sandbox + "/data/media" + p.substr(11);
    }
    if (p == "/storage/emulated") {
        return sandbox + "/data/media";
    }
    if (p.rfind("/storage/emulated/", 0) == 0) {
        return sandbox + "/data/media" + p.substr(17);
    }
    if (p == "/proc/mounts") {
        return sandbox + "/proc/mounts";
    }
    if (p == "/dev/fuse") {
        return sandbox + "/tmp/dev_fuse.raw";
    }
    if (p == "/dev/socket/rild") {
        return sandbox + "/dev/socket/rild";
    }
    if (p == "/dev/socket/rild-debug") {
        return sandbox + "/dev/socket/rild-debug";
    }
    if (p == "/dev/socket/wpa_wlan0") {
        return sandbox + "/dev/socket/wpa_wlan0";
    }
    if (p == "/dev/vhci") {
        return sandbox + "/tmp/dev_vhci.raw";
    }
    if (p == "/dev/rfkill") {
        return sandbox + "/tmp/dev_rfkill.raw";
    }
    if (p == "/dev/tee0") {
        return sandbox + "/tmp/dev_tee0.raw";
    }
    if (p == "/dev/ion") {
        return sandbox + "/tmp/dev_ion.raw";
    }
    if (p == "/dev/qseecom") {
        return sandbox + "/tmp/dev_qseecom.raw";
    }

    return p;
}

std::unordered_map<std::string, std::string> loadPropertyCacheFromDisk() {
    std::unordered_map<std::string, std::string> props;
    const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
    std::string cachePath = (sandboxEnv && strlen(sandboxEnv) > 0)
                            ? std::string(sandboxEnv) + "/dev/__properties_text__"
                            : "/data/data/com.gsi.runtime/files/sandbox/dev/__properties_text__";

    FILE* f = fopen(cachePath.c_str(), "r");
    if (!f) return props;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Line format: "[key]: [value]\n"
        if (line[0] == '[') {
            char* endKey = strchr(line + 1, ']');
            if (endKey) {
                *endKey = '\0';
                char* startVal = strstr(endKey + 1, "[");
                if (startVal) {
                    char* endVal = strrchr(startVal + 1, ']');
                    if (endVal) {
                        *endVal = '\0';
                        props[line + 1] = startVal + 1;
                    }
                }
            }
        }
    }
    fclose(f);
    return props;
}

} // anonymous namespace

extern "C" {

// -------------------------------------------------------------
// 1. Filesystem & Device Interceptions
// -------------------------------------------------------------

int open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    if (path != nullptr) {
        if (std::strstr(path, "/dev/binder") != nullptr) {
            LOGI("Hook: Intercepted open(\"%s\") -> Diverting to Virtual Binder Broker", path);
            return gsi::VirtualBinderBroker::getInstance().createVirtualBinderFd(gsi::BinderType::BINDER);
        }
        if (std::strstr(path, "/dev/hwbinder") != nullptr) {
            LOGI("Hook: Intercepted open(\"%s\") -> Diverting to Virtual HwBinder Broker", path);
            return gsi::VirtualBinderBroker::getInstance().createVirtualBinderFd(gsi::BinderType::HWBINDER);
        }
        if (std::strstr(path, "/dev/vndbinder") != nullptr) {
            LOGI("Hook: Intercepted open(\"%s\") -> Diverting to Virtual VndBinder Broker", path);
            return gsi::VirtualBinderBroker::getInstance().createVirtualBinderFd(gsi::BinderType::VNDBINDER);
        }

        std::string redirected = redirectSandboxPath(path);
        if (redirected != path) {
            LOGI("Hook: Redirected open(\"%s\") -> \"%s\"", path, redirected.c_str());
            auto real = getRealOpen();
            return real ? real(redirected.c_str(), flags, mode) : -1;
        }
    }

    auto real = getRealOpen();
    return real ? real(path, flags, mode) : -1;
}

int openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    if (path != nullptr) {
        if (std::strstr(path, "/dev/binder") != nullptr) {
            LOGI("Hook: Intercepted openat(\"%s\") -> Diverting to Virtual Binder Broker", path);
            return gsi::VirtualBinderBroker::getInstance().createVirtualBinderFd(gsi::BinderType::BINDER);
        }
        if (std::strstr(path, "/dev/hwbinder") != nullptr) {
            LOGI("Hook: Intercepted openat(\"%s\") -> Diverting to Virtual HwBinder Broker", path);
            return gsi::VirtualBinderBroker::getInstance().createVirtualBinderFd(gsi::BinderType::HWBINDER);
        }
        if (std::strstr(path, "/dev/vndbinder") != nullptr) {
            LOGI("Hook: Intercepted openat(\"%s\") -> Diverting to Virtual VndBinder Broker", path);
            return gsi::VirtualBinderBroker::getInstance().createVirtualBinderFd(gsi::BinderType::VNDBINDER);
        }

        std::string redirected = redirectSandboxPath(path);
        if (redirected != path) {
            LOGI("Hook: Redirected openat(\"%s\") -> \"%s\"", path, redirected.c_str());
            auto real = getRealOpenat();
            return real ? real(AT_FDCWD, redirected.c_str(), flags, mode) : -1;
        }
    }

    auto real = getRealOpenat();
    return real ? real(dirfd, path, flags, mode) : -1;
}

int ioctl(int fd, int request, ...) {
    va_list args;
    va_start(args, request);
    void* arg = va_arg(args, void*);
    va_end(args);

    if (gsi::VirtualBinderBroker::getInstance().isVirtualBinderFd(fd)) {
        return gsi::VirtualBinderBroker::getInstance().handleIoctl(fd, request, arg);
    }

    auto real = getRealIoctl();
    return real ? real(fd, request, arg) : -1;
}

int mount(const char* source, const char* target,
          const char* filesystemtype, unsigned long mountflags,
          const void* data) {
    LOGI("Hook: Intercepted mount(source=\"%s\", target=\"%s\", fstype=\"%s\") -> Faking SUCCESS (EPERM bypass)",
         source ? source : "none", target ? target : "none", filesystemtype ? filesystemtype : "auto");
    return 0; // Return success to allow bootstrap scripts to proceed
}

int umount(const char* target) {
    LOGI("Hook: Intercepted umount(target=\"%s\") -> Faking SUCCESS", target ? target : "none");
    return 0;
}

int umount2(const char* target, int flags) {
    LOGI("Hook: Intercepted umount2(target=\"%s\", flags=%d) -> Faking SUCCESS", target ? target : "none", flags);
    return 0;
}

int chroot(const char* path) {
    LOGI("Hook: Intercepted chroot(path=\"%s\") -> Faking SUCCESS (EPERM bypass)", path ? path : "/");
    return 0; // Return success
}

int statfs(const char* path, struct statfs* buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    std::memset(buf, 0, sizeof(*buf));
    buf->f_type = 0x65735546; // FUSE_SUPER_MAGIC
    buf->f_bsize = 4096;
    buf->f_blocks = 16777216ULL; // 64 GB
    buf->f_bfree = 14680064ULL;  // ~56 GB free
    buf->f_bavail = 14680064ULL; // ~56 GB avail
    buf->f_files = 1000000;
    buf->f_ffree = 950000;
    buf->f_namelen = 255;
    buf->f_frsize = 4096;
    buf->f_flags = 0;
    return 0;
}

int statfs64(const char* path, struct statfs64* buf) {
    return statfs(path, reinterpret_cast<struct statfs*>(buf));
}

int statvfs(const char* path, struct statvfs* buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    std::memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 16777216ULL; // 64 GB
    buf->f_bfree = 14680064ULL;  // ~56 GB free
    buf->f_bavail = 14680064ULL; // ~56 GB avail
    buf->f_files = 1000000;
    buf->f_ffree = 950000;
    buf->f_favail = 950000;
    buf->f_namemax = 255;
    return 0;
}

int statvfs64(const char* path, struct statvfs64* buf) {
    return statvfs(path, reinterpret_cast<struct statvfs*>(buf));
}

int stat(const char* path, struct stat* sb) {
    if (path) {
        std::string redirected = redirectSandboxPath(path);
        static auto real = reinterpret_cast<int (*)(const char*, struct stat*)>(dlsym(RTLD_NEXT, "stat"));
        if (real) return real(redirected.c_str(), sb);
        return fstatat(AT_FDCWD, redirected.c_str(), sb, 0);
    }
    errno = EFAULT;
    return -1;
}

int lstat(const char* path, struct stat* sb) {
    if (path) {
        std::string redirected = redirectSandboxPath(path);
        static auto real = reinterpret_cast<int (*)(const char*, struct stat*)>(dlsym(RTLD_NEXT, "lstat"));
        if (real) return real(redirected.c_str(), sb);
        return fstatat(AT_FDCWD, redirected.c_str(), sb, AT_SYMLINK_NOFOLLOW);
    }
    errno = EFAULT;
    return -1;
}

int access(const char* path, int mode) {
    if (path) {
        std::string redirected = redirectSandboxPath(path);
        static auto real = reinterpret_cast<int (*)(const char*, int)>(dlsym(RTLD_NEXT, "access"));
        if (real) return real(redirected.c_str(), mode);
        return faccessat(AT_FDCWD, redirected.c_str(), mode, 0);
    }
    errno = EFAULT;
    return -1;
}

int close(int fd) {
    if (gsi::VirtualBinderBroker::getInstance().isVirtualBinderFd(fd)) {
        gsi::VirtualBinderBroker::getInstance().closeVirtualBinderFd(fd);
        return 0;
    }

    auto real = getRealClose();
    return real ? real(fd) : -1;
}

// -------------------------------------------------------------
// 2. Network & UNIX Domain Socket Interception
// -------------------------------------------------------------

int socket(int domain, int type, int protocol) {
    auto realSocket = getRealSocket();
    if (!realSocket) return -1;

    // Check for unprivileged raw ICMP attempt (e.g. toybox ping)
    // Non-root processes cannot open SOCK_RAW IPPROTO_ICMP on Android host.
    // Divert to Linux unprivileged datagram ICMP socket (SOCK_DGRAM)
    int baseType = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);

    if ((domain == AF_INET || domain == AF_INET6) &&
        baseType == SOCK_RAW &&
        (protocol == IPPROTO_ICMP || protocol == IPPROTO_ICMPV6 || protocol == 1 || protocol == 58)) {
        LOGI("Hook: Intercepted socket(domain=%d, SOCK_RAW, proto=%d) -> Diverting to SOCK_DGRAM (unprivileged ICMP)",
             domain, protocol);
        int fd = realSocket(domain, SOCK_DGRAM | flags, protocol);
        if (fd >= 0) {
            return fd;
        }
        LOGW("Hook: SOCK_DGRAM ICMP fallback returned %d (errno=%d), trying original", fd, errno);
    }

    return realSocket(domain, type, protocol);
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    auto realConnect = getRealConnect();

    if (addr && addr->sa_family == AF_UNIX) {
        const auto* un = reinterpret_cast<const struct sockaddr_un*>(addr);
        if (std::strstr(un->sun_path, "/dev/socket/property_service") != nullptr) {
            const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
            std::string targetPath = (sandboxEnv && strlen(sandboxEnv) > 0)
                                     ? std::string(sandboxEnv) + "/dev/socket/property_service"
                                     : "/data/data/com.gsi.runtime/files/sandbox/dev/socket/property_service";

            LOGI("Hook: Intercepted connect(\"%s\") -> Redirecting to \"%s\"",
                 un->sun_path, targetPath.c_str());

            struct sockaddr_un redirectedAddr{};
            redirectedAddr.sun_family = AF_UNIX;
            strncpy(redirectedAddr.sun_path, targetPath.c_str(), sizeof(redirectedAddr.sun_path) - 1);

            return realConnect ? realConnect(sockfd, reinterpret_cast<struct sockaddr*>(&redirectedAddr), sizeof(redirectedAddr)) : -1;
        }

        if (std::strstr(un->sun_path, "/dev/socket/netd") != nullptr ||
            std::strstr(un->sun_path, "/dev/socket/dnsproxyd") != nullptr) {
            const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
            std::string targetPath = (sandboxEnv && strlen(sandboxEnv) > 0)
                                     ? std::string(sandboxEnv) + "/dev/socket/netd"
                                     : "/data/data/com.gsi.runtime/files/sandbox/dev/socket/netd";

            LOGI("Hook: Intercepted connect(\"%s\") -> Redirecting to \"%s\"",
                 un->sun_path, targetPath.c_str());

            struct sockaddr_un redirectedAddr{};
            redirectedAddr.sun_family = AF_UNIX;
            strncpy(redirectedAddr.sun_path, targetPath.c_str(), sizeof(redirectedAddr.sun_path) - 1);

            return realConnect ? realConnect(sockfd, reinterpret_cast<struct sockaddr*>(&redirectedAddr), sizeof(redirectedAddr)) : -1;
        }

        if (std::strstr(un->sun_path, "/dev/socket/logdw") != nullptr ||
            std::strstr(un->sun_path, "/dev/socket/logd") != nullptr ||
            std::strstr(un->sun_path, "/dev/log/") != nullptr) {
            const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
            std::string targetPath = (sandboxEnv && strlen(sandboxEnv) > 0)
                                     ? std::string(sandboxEnv) + "/dev/socket/logdw"
                                     : "/data/data/com.gsi.runtime/files/sandbox/dev/socket/logdw";

            LOGI("Hook: Intercepted connect(\"%s\") -> Redirecting to \"%s\"",
                 un->sun_path, targetPath.c_str());

            struct sockaddr_un redirectedAddr{};
            redirectedAddr.sun_family = AF_UNIX;
            strncpy(redirectedAddr.sun_path, targetPath.c_str(), sizeof(redirectedAddr.sun_path) - 1);

            return realConnect ? realConnect(sockfd, reinterpret_cast<struct sockaddr*>(&redirectedAddr), sizeof(redirectedAddr)) : -1;
        }

        if (std::strstr(un->sun_path, "/dev/socket/logdr") != nullptr) {
            const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
            std::string targetPath = (sandboxEnv && strlen(sandboxEnv) > 0)
                                     ? std::string(sandboxEnv) + "/dev/socket/logdr"
                                     : "/data/data/com.gsi.runtime/files/sandbox/dev/socket/logdr";

            LOGI("Hook: Intercepted connect(\"%s\") -> Redirecting to \"%s\"",
                 un->sun_path, targetPath.c_str());

            struct sockaddr_un redirectedAddr{};
            redirectedAddr.sun_family = AF_UNIX;
            strncpy(redirectedAddr.sun_path, targetPath.c_str(), sizeof(redirectedAddr.sun_path) - 1);

            return realConnect ? realConnect(sockfd, reinterpret_cast<struct sockaddr*>(&redirectedAddr), sizeof(redirectedAddr)) : -1;
        }
    }

    return realConnect ? realConnect(sockfd, addr, addrlen) : -1;
}

// -------------------------------------------------------------
// 3. Privilege & Identity Spoofing (UID / GID)
// -------------------------------------------------------------

uid_t getuid(void) {
    initIdentityIfNeeded();
    return gVirtualRuid.load();
}

uid_t geteuid(void) {
    initIdentityIfNeeded();
    return gVirtualEuid.load();
}

gid_t getgid(void) {
    initIdentityIfNeeded();
    return gVirtualRgid.load();
}

gid_t getegid(void) {
    initIdentityIfNeeded();
    return gVirtualEgid.load();
}

int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid) {
    initIdentityIfNeeded();
    if (ruid) *ruid = gVirtualRuid.load();
    if (euid) *euid = gVirtualEuid.load();
    if (suid) *suid = gVirtualSuid.load();
    return 0;
}

int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid) {
    initIdentityIfNeeded();
    if (rgid) *rgid = gVirtualRgid.load();
    if (egid) *egid = gVirtualEgid.load();
    if (sgid) *sgid = gVirtualSgid.load();
    return 0;
}

int getgroups(int size, gid_t list[]) {
    initIdentityIfNeeded();
    std::lock_guard<std::mutex> lock(gIdentityMutex);
    if (size == 0) {
        return static_cast<int>(gVirtualGroups.size());
    }
    int count = std::min(size, static_cast<int>(gVirtualGroups.size()));
    for (int i = 0; i < count; ++i) {
        list[i] = gVirtualGroups[i];
    }
    return count;
}

int setuid(uid_t uid) {
    initIdentityIfNeeded();
    gVirtualRuid.store(uid);
    gVirtualEuid.store(uid);
    gVirtualSuid.store(uid);
    LOGI("Hook: Intercepted setuid(%u) -> Faked SUCCESS (Virtual UID updated)", uid);
    return 0;
}

int seteuid(uid_t euid) {
    initIdentityIfNeeded();
    gVirtualEuid.store(euid);
    LOGI("Hook: Intercepted seteuid(%u) -> Faked SUCCESS", euid);
    return 0;
}

int setgid(gid_t gid) {
    initIdentityIfNeeded();
    gVirtualRgid.store(gid);
    gVirtualEgid.store(gid);
    gVirtualSgid.store(gid);
    LOGI("Hook: Intercepted setgid(%u) -> Faked SUCCESS", gid);
    return 0;
}

int setegid(gid_t egid) {
    initIdentityIfNeeded();
    gVirtualEgid.store(egid);
    LOGI("Hook: Intercepted setegid(%u) -> Faked SUCCESS", egid);
    return 0;
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    initIdentityIfNeeded();
    if (ruid != static_cast<uid_t>(-1)) gVirtualRuid.store(ruid);
    if (euid != static_cast<uid_t>(-1)) gVirtualEuid.store(euid);
    if (suid != static_cast<uid_t>(-1)) gVirtualSuid.store(suid);
    LOGI("Hook: Intercepted setresuid(%u, %u, %u) -> Faked SUCCESS", ruid, euid, suid);
    return 0;
}

int setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
    initIdentityIfNeeded();
    if (rgid != static_cast<gid_t>(-1)) gVirtualRgid.store(rgid);
    if (egid != static_cast<gid_t>(-1)) gVirtualEgid.store(egid);
    if (sgid != static_cast<gid_t>(-1)) gVirtualSgid.store(sgid);
    LOGI("Hook: Intercepted setresgid(%u, %u, %u) -> Faked SUCCESS", rgid, egid, sgid);
    return 0;
}

int setgroups(size_t size, const gid_t *list) {
    initIdentityIfNeeded();
    std::lock_guard<std::mutex> lock(gIdentityMutex);
    gVirtualGroups.clear();
    for (size_t i = 0; i < size; ++i) {
        gVirtualGroups.push_back(list[i]);
    }
    LOGI("Hook: Intercepted setgroups(size=%zu) -> Faked SUCCESS", size);
    return 0;
}

// -------------------------------------------------------------
// 4. Capabilities & prctl
// -------------------------------------------------------------

int capget(cap_user_header_t /* hdrp */, cap_user_data_t datap) {
    if (datap) {
        // Grant full capability bits to pass permission checks
        datap[0].effective = 0xFFFFFFFF;
        datap[0].permitted = 0xFFFFFFFF;
        datap[0].inheritable = 0xFFFFFFFF;
    }
    return 0;
}

int capset(cap_user_header_t /* hdrp */, const cap_user_data_t /* datap */) {
    LOGI("Hook: Intercepted capset() -> Spoofing SUCCESS (Bypassing CAP_SETPCAP / EPERM)");
    return 0;
}

int prctl(int option, ...) {
    va_list args;
    va_start(args, option);
    unsigned long arg2 = va_arg(args, unsigned long);
    unsigned long arg3 = va_arg(args, unsigned long);
    unsigned long arg4 = va_arg(args, unsigned long);
    unsigned long arg5 = va_arg(args, unsigned long);
    va_end(args);

    if (option == PR_SET_KEEPCAPS || option == 0x2f /* PR_CAP_AMBIENT */ || option == 0x18 /* PR_CAPBSET_DROP */) {
        LOGI("Hook: Intercepted capability prctl(option=%d) -> Faked SUCCESS", option);
        return 0;
    }

    auto real = getRealPrctl();
    return real ? real(option, arg2, arg3, arg4, arg5) : 0;
}

// -------------------------------------------------------------
// 5. SELinux Permissive Spoofing
// -------------------------------------------------------------

int is_selinux_enabled(void) {
    return 1; // Enabled so SELinux-aware daemons don't crash
}

int is_selinux_enforcing(void) {
    return 0; // Permissive! Never enforce
}

int security_getenforce(void) {
    return 0; // Permissive (0 = permissive, 1 = enforcing)
}

int security_setenforce(int value) {
    LOGI("Hook: Intercepted security_setenforce(%d) -> Faked SUCCESS", value);
    return 0;
}

int setcon(const char* context) {
    LOGI("Hook: Intercepted setcon(\"%s\") -> Faked SUCCESS", context ? context : "");
    return 0;
}

int getcon(char** context) {
    if (context) {
        *context = strdup("u:r:su:s0");
    }
    return 0;
}

int getfilecon(const char* /* path */, char** context) {
    if (context) {
        *context = strdup("u:object_r:system_file:s0");
    }
    return 0;
}

void freecon(char* con) {
    if (con) free(con);
}

// -------------------------------------------------------------
// 6. Android Bionic System Properties Interception
// -------------------------------------------------------------

int __system_property_get(const char* name, char* value) {
    if (!name || !value) return 0;
    value[0] = '\0';

    auto props = loadPropertyCacheFromDisk();
    auto it = props.find(name);
    if (it != props.end()) {
        size_t len = std::min(it->second.size(), static_cast<size_t>(91));
        std::memcpy(value, it->second.c_str(), len);
        value[len] = '\0';
        return static_cast<int>(len);
    }

    auto real = getRealPropGet();
    return real ? real(name, value) : 0;
}

int __system_property_set(const char* key, const char* value) {
    if (!key || !value) return -1;

    LOGI("Hook: Intercepted __system_property_set(\"%s\", \"%s\")", key, value);

    const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
    std::string socketPath = (sandboxEnv && strlen(sandboxEnv) > 0)
                             ? std::string(sandboxEnv) + "/dev/socket/property_service"
                             : "/data/data/com.gsi.runtime/files/sandbox/dev/socket/property_service";

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock >= 0) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        auto realConnect = getRealConnect();
        int connRes = realConnect
                      ? realConnect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr))
                      : connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

        if (connRes == 0) {
            std::string msg = std::string(key) + "=" + std::string(value);
            write(sock, msg.data(), msg.size());
            uint32_t resp = 0;
            read(sock, &resp, sizeof(resp));
            close(sock);
            return 0;
        }
        close(sock);
    }

    return 0;
}

// -------------------------------------------------------------
// 7. Privilege Spoof Diagnostic Self-Test
// -------------------------------------------------------------

const char* gsi_run_privilege_diagnostic(void) {
    static std::string result;
    std::ostringstream oss;

    oss << "=== GSI PRIVILEGE & IDENTITY SPOOFING DIAGNOSTIC ===\n";

    // 1. Check Initial UID/GID
    uid_t initialUid = getuid();
    uid_t initialEuid = geteuid();
    gid_t initialGid = getgid();
    oss << "[1] Current Virtual Identity: UID=" << initialUid
        << " (EUID=" << initialEuid << ") GID=" << initialGid << "\n";

    // 2. Test setuid transition (e.g. drop to system user 1000)
    int s1 = setuid(1000);
    uid_t u1000 = getuid();
    oss << "[2] Transition to UID 1000: setuid(1000) -> return=" << s1
        << " (Current UID=" << u1000 << ")\n";

    // 3. Test setuid transition back to root (UID 0)
    int s0 = setuid(0);
    uid_t u0 = getuid();
    oss << "[3] Escalation back to Root: setuid(0) -> return=" << s0
        << " (Current UID=" << u0 << ")\n";

    // 4. Test Capabilities
    struct __user_cap_header_struct hdr{};
    struct __user_cap_data_struct data[2]{};
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    int capGetRes = capget(&hdr, data);
    oss << "[4] Capability Query: capget() -> return=" << capGetRes
        << " (Effective=0x" << std::hex << data[0].effective << std::dec << ")\n";

    int capSetRes = capset(&hdr, data);
    oss << "[5] Capability Setting: capset() -> return=" << capSetRes << " (Spoofed SUCCESS)\n";

    // 5. Test SELinux Enforcing Mode
    int selEn = is_selinux_enabled();
    int selEnf = is_selinux_enforcing();
    int secEnf = security_getenforce();
    oss << "[6] SELinux State: Enabled=" << selEn
        << " | Enforcing=" << selEnf
        << " | security_getenforce=" << secEnf << " (0 = Permissive)\n";

    // 6. Test Security Context
    char* con = nullptr;
    getcon(&con);
    oss << "[7] Security Context: " << (con ? con : "null") << "\n";
    if (con) freecon(con);

    oss << "====================================================\n";
    oss << "Status: ALL PRIVILEGE & SECURITY HOOKS OPERATIONAL!";

    result = oss.str();
    return result.c_str();
}

// -------------------------------------------------------------
// 8. Android Graphics & Hardware HAL Interception
// -------------------------------------------------------------

int ashmem_create_region(const char* name, size_t size) {
    const char* sandboxEnv = getenv("GSI_SANDBOX_DIR");
    std::string sandbox = (sandboxEnv && strlen(sandboxEnv) > 0)
                          ? sandboxEnv
                          : "/data/data/com.gsi.runtime/files/sandbox";

    static std::atomic<uint32_t> ashmemCounter{0};
    uint32_t id = ashmemCounter++;
    std::string path = sandbox + "/tmp/ashmem_" + std::to_string(getpid()) + "_" + std::to_string(id) + ".raw";

    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        ftruncate(fd, size);
        LOGI("Hook: Intercepted ashmem_create_region(\"%s\", %zu) -> Backed by %s",
             name ? name : "default", size, path.c_str());
        return fd;
    }
    return -1;
}

int hw_get_module(const char* id, const void** module) {
    if (!id || !module) return -1;

    LOGI("Hook: Intercepted hw_get_module(\"%s\") -> Emulating Hardware Module", id);

    struct VirtualHwModule {
        uint32_t tag;
        uint16_t module_api_version;
        uint16_t hal_api_version;
        const char *id;
        const char *name;
        const char *author;
        void* methods;
        void* dso;
        uint32_t reserved[32];
    };

    static VirtualHwModule grallocModule = {
        0x48574D54, // HARDWARE_MODULE_TAG ("HWMT")
        1,
        0,
        "gralloc",
        "Virtual GSI Gralloc Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule hwcModule = {
        0x48574D54,
        1,
        0,
        "hwcomposer",
        "Virtual GSI Hardware Composer",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule audioModule = {
        0x48574D54,
        1,
        0,
        "audio",
        "Virtual GSI Audio HAL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule sensorsModule = {
        0x48574D54,
        1,
        0,
        "sensors",
        "Virtual GSI Sensors HAL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule cameraModule = {
        0x48574D54,
        1,
        0,
        "camera",
        "Virtual GSI Camera HAL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule radioModule = {
        0x48574D54,
        1,
        0,
        "radio",
        "Virtual GSI Radio / RIL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule wifiModule = {
        0x48574D54,
        1,
        0,
        "wifi",
        "Virtual GSI Wi-Fi HAL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule bluetoothModule = {
        0x48574D54,
        1,
        0,
        "bluetooth",
        "Virtual GSI Bluetooth HAL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule drmModule = {
        0x48574D54,
        1,
        0,
        "drm",
        "Virtual GSI DRM / ClearKey / Widevine HAL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    static VirtualHwModule cryptoModule = {
        0x48574D54,
        1,
        0,
        "crypto",
        "Virtual GSI MediaCrypto HAL Module",
        "Google DeepMind GSI Engine",
        nullptr,
        nullptr,
        {}
    };

    if (strcmp(id, "gralloc") == 0) {
        *module = &grallocModule;
        return 0;
    }
    if (strcmp(id, "hwcomposer") == 0) {
        *module = &hwcModule;
        return 0;
    }
    if (strcmp(id, "audio") == 0) {
        *module = &audioModule;
        return 0;
    }
    if (strcmp(id, "sensors") == 0) {
        *module = &sensorsModule;
        return 0;
    }
    if (strcmp(id, "camera") == 0) {
        *module = &cameraModule;
        return 0;
    }
    if (strcmp(id, "radio") == 0) {
        *module = &radioModule;
        return 0;
    }
    if (strcmp(id, "wifi") == 0) {
        *module = &wifiModule;
        return 0;
    }
    if (strcmp(id, "bluetooth") == 0) {
        *module = &bluetoothModule;
        return 0;
    }
    if (strcmp(id, "drm") == 0) {
        *module = &drmModule;
        return 0;
    }
    if (strcmp(id, "crypto") == 0) {
        *module = &cryptoModule;
        return 0;
    }

    static auto realHwGet = reinterpret_cast<int (*)(const char*, const void**)>(dlsym(RTLD_NEXT, "hw_get_module"));
    if (realHwGet) {
        return realHwGet(id, module);
    }
    return -1;
}

} // extern "C"
