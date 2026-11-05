/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-only access() interception: lets tests simulate /etc/camera and
 * /system/etc/camera existence without touching the real filesystem. */
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <map>
#include <string>

namespace {
std::map<std::string, bool> &entries()
{
    static std::map<std::string, bool> values;
    return values;
}
}  // namespace

extern "C" void host_fs_set_exists(const char *path, bool exists)
{
    if (path == nullptr) return;
    entries()[path] = exists;
}

extern "C" void host_fs_clear_all(void)
{
    entries().clear();
}

extern "C" int access(const char *path, int mode)
{
    auto it = entries().find(path);
    if (it != entries().end()) {
        if (it->second) return 0;
        errno = ENOENT;
        return -1;
    }
    return int(syscall(SYS_access, path, mode));
}
