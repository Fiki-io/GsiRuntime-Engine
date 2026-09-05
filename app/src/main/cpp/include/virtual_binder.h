#pragma once

#include <sys/ioctl.h>
#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <vector>

namespace gsi {

constexpr int32_t BINDER_CURRENT_PROTOCOL_VERSION = 8;

#pragma pack(push, 1)
struct BinderVersion {
    int32_t protocol_version;
};

struct BinderWriteRead {
    uint64_t write_size;
    uint64_t write_consumed;
    uint64_t write_buffer;
    uint64_t read_size;
    uint64_t read_consumed;
    uint64_t read_buffer;
};
#pragma pack(pop)

// Binder ioctl command codes (Android Linux kernel)
#define V_BINDER_WRITE_READ          _IOWR('b', 1, struct gsi::BinderWriteRead)
#define V_BINDER_SET_MAX_THREADS     _IOW('b', 5, uint32_t)
#define V_BINDER_SET_CONTEXT_MGR     _IOW('b', 7, int32_t)
#define V_BINDER_THREAD_EXIT         _IOW('b', 8, int32_t)
#define V_BINDER_VERSION             _IOWR('b', 9, struct gsi::BinderVersion)

enum class BinderType {
    BINDER = 1,
    HWBINDER = 2,
    VNDBINDER = 3
};

struct VirtualBinderDescriptor {
    int fd;
    BinderType type;
    uint32_t maxThreads = 16;
    bool isContextManager = false;
};

class VirtualBinderBroker {
public:
    static VirtualBinderBroker& getInstance();

    int createVirtualBinderFd(BinderType type);
    bool isVirtualBinderFd(int fd) const;
    void closeVirtualBinderFd(int fd);

    int handleIoctl(int fd, int request, void* arg);

    // Diagnostics
    std::string runSelfTest();

private:
    VirtualBinderBroker() = default;

    mutable std::mutex mMutex;
    std::map<int, VirtualBinderDescriptor> mDescriptors;
    std::map<std::string, uint64_t> mServiceRegistry;
};

} // namespace gsi
