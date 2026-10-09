/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_ALGORITHM_CPU_SCOPE_H
#define QCAMERA3_ALGORITHM_CPU_SCOPE_H

#include <sched.h>
#include <cstring>
#include <cstdio>
#include <errno.h>

namespace qcamera {

class QCamera3AlgorithmCpuScope {
public:
    explicit QCamera3AlgorithmCpuScope(bool enabled = true) {
        if (!enabled) return;
        mStatus = readAllowed(mOriginal);
        if (mStatus != 0) {
            return;
        }
        cpu_set_t preferred;
        CPU_ZERO(&preferred);
        bool available = false;
        // R11/R11s use SDM660: CPUs 4–7 are the performance cluster.
        for (int cpu = 4; cpu < 8; ++cpu) if (CPU_ISSET(cpu, &mOriginal)) {
            CPU_SET(cpu, &preferred);
            mStatus |= 1 << cpu;
            available = true;
        }
        if (available && memcmp(&preferred, &mOriginal, sizeof(preferred)) != 0) {
            mChanged = sched_setaffinity(0, sizeof(preferred), &preferred) == 0;
            if (!mChanged) mStatus = -errno;
        }
    }
    ~QCamera3AlgorithmCpuScope() {
        if (mChanged) sched_setaffinity(0, sizeof(mOriginal), &mOriginal);
    }
    QCamera3AlgorithmCpuScope(const QCamera3AlgorithmCpuScope &) = delete;
    QCamera3AlgorithmCpuScope &operator=(const QCamera3AlgorithmCpuScope &) = delete;
    int status() const { return mStatus; }

private:
    static int readAllowed(cpu_set_t &mask) {
        // This kernel's getaffinity hides isolated/offline CPUs; restoring it
        // would permanently remove those CPUs from the thread's allowed mask.
        FILE *file = fopen("/proc/thread-self/status", "re");
        if (!file) return -errno;
        char line[512];
        int status = -EINVAL;
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, "Cpus_allowed:", 13) != 0) continue;
            const char *first = line + 13;
            while (*first == ' ' || *first == '\t') ++first;
            const char *last = first + strcspn(first, "\r\n");
            if (*last == '\0' || first == last) break;
            CPU_ZERO(&mask);
            size_t cpu = 0;
            unsigned groupDigits = 0;
            bool valid = true;
            for (const char *digit = last; digit != first;) {
                const char c = *--digit;
                if (c == ',') {
                    if (groupDigits != 8) { valid = false; break; }
                    groupDigits = 0;
                    continue;
                }
                const int nibble = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (nibble < 0 || ++groupDigits > 8 || cpu + 4 > CPU_SETSIZE) {
                    valid = false; break;
                }
                for (unsigned bit = 0; bit < 4; ++bit)
                    if (nibble & (1 << bit)) CPU_SET(cpu + bit, &mask);
                cpu += 4;
            }
            if (valid && groupDigits != 0 && CPU_COUNT(&mask) != 0) status = 0;
            break;
        }
        fclose(file);
        return status;
    }
    cpu_set_t mOriginal = {};
    bool mChanged = false;
    int mStatus = 0;
};

} // namespace qcamera
#endif
