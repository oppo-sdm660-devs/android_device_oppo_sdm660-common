/* SPDX-License-Identifier: BSD-3-Clause */
/* Shared control block for the fake liboppo_platform_hwi.so used by host tests. */
#ifndef FAKE_OPPO_PLATFORM_HWI_H
#define FAKE_OPPO_PLATFORM_HWI_H

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_OPPO_PARAM_SIZE 0x12EC
#define FAKE_OPPO_CTX_SIZE 0x308

struct FakeOppoPlatformStats {
    int initCount;
    int processCount;
    int updateCount;
    int uninitCount;
    unsigned long long initMask;
    unsigned long long processMask;
    unsigned long long updateMask;
    int processRc;
    unsigned char initParam[FAKE_OPPO_PARAM_SIZE];
    unsigned char processParam[FAKE_OPPO_PARAM_SIZE];
    unsigned char frameCtx[FAKE_OPPO_CTX_SIZE];
};

struct FakeOppoPlatformStats *fake_oppo_platform_stats(void);
void fake_oppo_platform_reset(void);
void fake_oppo_platform_set_process_rc(int rc);
void fake_oppo_platform_set_init_rc(int rc);
void fake_oppo_platform_set_mutate(int value);

#ifdef __cplusplus
}
#endif

#endif
