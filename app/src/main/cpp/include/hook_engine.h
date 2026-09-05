#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <linux/capability.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1. Filesystem & Device Interception
__attribute__((visibility("default"))) int open(const char* path, int flags, ...);
__attribute__((visibility("default"))) int openat(int dirfd, const char* path, int flags, ...);
__attribute__((visibility("default"))) int ioctl(int fd, int request, ...);
__attribute__((visibility("default"))) int mount(const char* source, const char* target,
                                                const char* filesystemtype, unsigned long mountflags,
                                                const void* data);
__attribute__((visibility("default"))) int chroot(const char* path);
__attribute__((visibility("default"))) int close(int fd);

// 2. Network & UNIX Domain Socket Interception
__attribute__((visibility("default"))) int socket(int domain, int type, int protocol);
__attribute__((visibility("default"))) int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);

// 3. Privilege & Identity Spoofing (UID / GID)
__attribute__((visibility("default"))) uid_t getuid(void);
__attribute__((visibility("default"))) uid_t geteuid(void);
__attribute__((visibility("default"))) gid_t getgid(void);
__attribute__((visibility("default"))) gid_t getegid(void);
__attribute__((visibility("default"))) int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
__attribute__((visibility("default"))) int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
__attribute__((visibility("default"))) int getgroups(int size, gid_t list[]);

__attribute__((visibility("default"))) int setuid(uid_t uid);
__attribute__((visibility("default"))) int seteuid(uid_t euid);
__attribute__((visibility("default"))) int setgid(gid_t gid);
__attribute__((visibility("default"))) int setegid(gid_t egid);
__attribute__((visibility("default"))) int setresuid(uid_t ruid, uid_t euid, uid_t suid);
__attribute__((visibility("default"))) int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
__attribute__((visibility("default"))) int setgroups(size_t size, const gid_t *list);

// 4. Linux Capabilities & Process Control
__attribute__((visibility("default"))) int capget(cap_user_header_t hdrp, cap_user_data_t datap);
__attribute__((visibility("default"))) int capset(cap_user_header_t hdrp, const cap_user_data_t datap);
__attribute__((visibility("default"))) int prctl(int option, ...);

// 5. SELinux Permissive Spoofing
__attribute__((visibility("default"))) int is_selinux_enabled(void);
__attribute__((visibility("default"))) int is_selinux_enforcing(void);
__attribute__((visibility("default"))) int security_getenforce(void);
__attribute__((visibility("default"))) int security_setenforce(int value);
__attribute__((visibility("default"))) int setcon(const char *context);
__attribute__((visibility("default"))) int getcon(char **context);
__attribute__((visibility("default"))) int getfilecon(const char *path, char **context);
__attribute__((visibility("default"))) void freecon(char *con);

// 6. Android Bionic System Properties Interception
__attribute__((visibility("default"))) int __system_property_get(const char* name, char* value);
__attribute__((visibility("default"))) int __system_property_set(const char* key, const char* value);

// 7. Android Graphics & Hardware HAL Interception
__attribute__((visibility("default"))) int ashmem_create_region(const char* name, size_t size);
__attribute__((visibility("default"))) int hw_get_module(const char* id, const void** module);

// 8. Engine Diagnostic Self-Test
const char* gsi_run_privilege_diagnostic(void);

#ifdef __cplusplus
}
#endif
