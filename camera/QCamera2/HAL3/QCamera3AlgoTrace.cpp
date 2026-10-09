/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3AlgoTrace.h"

#include <string.h>

namespace qcamera {

void QCamera3AlgoTrace::reset() {
    mBuffer[0] = '\0';
    mLength = 0;
    mStages = 0;
}

bool QCamera3AlgoTrace::add(const char *name, int32_t status, uint32_t frame,
        uint32_t inputs) {
    if (name == nullptr || name[0] == '\0') return false;
    if (mStages >= kMaxStages) return false;

    // Hand-rolled formatting; no snprintf on this hot path.
    char stage[kMaxLength + 1];
    size_t used = 0;
    const char prefix[] = "OPPO_HAL3 algorithm=";
    for (size_t i = 0; i < sizeof(prefix) - 1 && used < kMaxLength; i++) {
        stage[used++] = prefix[i];
    }
    for (size_t i = 0; name[i] != '\0' && used < kMaxLength; i++) {
        stage[used++] = name[i];
    }
    static const char statusKey[] = " status=";
    static const char frameKey[] = " frame=";
    static const char inputsKey[] = " inputs=";
    const struct { const char *key; size_t keyLength; int64_t value; } fields[] = {
        { statusKey, sizeof(statusKey) - 1, status },
        { frameKey, sizeof(frameKey) - 1, (int64_t)frame },
        { inputsKey, sizeof(inputsKey) - 1, (int64_t)inputs },
    };
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        for (size_t i = 0; i < fields[f].keyLength && used < kMaxLength; i++) {
            stage[used++] = fields[f].key[i];
        }
        char digits[24];
        size_t count = 0;
        int64_t value = fields[f].value;
        bool negative = value < 0;
        uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
        do {
            digits[count++] = (char)('0' + (magnitude % 10u));
            magnitude /= 10u;
        } while (magnitude != 0 && count < sizeof(digits));
        if (negative && used < kMaxLength) stage[used++] = '-';
        while (count > 0 && used < kMaxLength) {
            stage[used++] = digits[--count];
        }
    }
    stage[used] = '\0';

    const size_t separator = (mStages == 0) ? 0 : 1;
    if (mLength + separator + used > kMaxLength) return false;
    if (separator != 0) mBuffer[mLength++] = ';';
    memcpy(mBuffer + mLength, stage, used);
    mLength += used;
    mBuffer[mLength] = '\0';
    mStages++;
    return true;
}

} // namespace qcamera
