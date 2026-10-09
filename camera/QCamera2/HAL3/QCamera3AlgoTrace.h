/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_ALGO_TRACE_H
#define QCAMERA3_ALGO_TRACE_H

#include <stddef.h>
#include <stdint.h>

namespace qcamera {

// Format: OPPO_HAL3 algorithm=<Name> status=<n> frame=<n> inputs=<count>,
//   stages joined with ';'; also exposed as org.lineageos.camera.algorithms.nativeTrace.
class QCamera3AlgoTrace {
public:
    static const size_t kMaxStages = 8;
    static const size_t kMaxLength = 512;

    void reset();
    // Returns false without modifying the trace.
    bool add(const char *name, int32_t status, uint32_t frame, uint32_t inputs);
    const char *c_str() const { return mBuffer; }
    size_t length() const { return mLength; }
    size_t stages() const { return mStages; }
    bool empty() const { return mStages == 0; }

private:
    char mBuffer[kMaxLength + 1] = {};
    size_t mLength = 0;
    size_t mStages = 0;
};

} // namespace qcamera
#endif
