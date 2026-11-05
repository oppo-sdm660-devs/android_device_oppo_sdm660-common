/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Host fake of liboppo_platform_hwi.so. Exports the exact C++ mangled entry
 * points the HAL wrapper resolves with dlsym and records every call so the host
 * test can assert the reversed ABI/order/mask/param layout. No algorithm runs.
 */
#include "fake_oppo_platform_hwi.h"

#include <string.h>

namespace {
FakeOppoPlatformStats g_stats = {};
int g_initRc = 0;
}  // namespace

extern "C" int fake_oppo_platform_init(void *p1, unsigned long long mask,
        unsigned long long *out1, unsigned long long *out2, unsigned long long *out3)
        asm("_Z18oppo_platform_initPvyPyS0_S0_");
extern "C" int fake_oppo_platform_process(void *frameCtx, void *param,
        unsigned long long mask) asm("_Z21oppo_platform_processPvS_y");
extern "C" int fake_oppo_platform_uninit(void *p1, unsigned long long mask)
        asm("_Z20oppo_platform_uninitPvy");
extern "C" int fake_oppo_platform_updateparameters(void *p1, void *p2,
        unsigned long long mask) asm("_Z30oppo_platform_updateparametersPvS_y");

extern "C" int fake_oppo_platform_init(void *p1, unsigned long long mask,
        unsigned long long *out1, unsigned long long *out2, unsigned long long *out3)
{
    ++g_stats.initCount;
    g_stats.initMask = mask;
    memset(g_stats.initParam, 0, sizeof(g_stats.initParam));
    if (p1 != nullptr)
        memcpy(g_stats.initParam, p1, sizeof(g_stats.initParam));
    if (out1 != nullptr) *out1 = 0;
    if (out2 != nullptr) *out2 = 0;
    if (out3 != nullptr) *out3 = 0;
    return g_initRc;
}

extern "C" int fake_oppo_platform_process(void *frameCtx, void *param,
        unsigned long long mask)
{
    ++g_stats.processCount;
    g_stats.processMask = mask;
    if (param != nullptr)
        memcpy(g_stats.processParam, param, sizeof(g_stats.processParam));
    if (frameCtx != nullptr)
        memcpy(g_stats.frameCtx, frameCtx, sizeof(g_stats.frameCtx));
    return g_stats.processRc;
}

extern "C" int fake_oppo_platform_updateparameters(void *p1, void *p2,
        unsigned long long mask)
{
    ++g_stats.updateCount;
    g_stats.updateMask = mask;
    if (p1 != nullptr)
        memcpy(g_stats.processParam, p1, sizeof(g_stats.processParam));
    else if (p2 != nullptr)
        memcpy(g_stats.processParam, p2, sizeof(g_stats.processParam));
    return 0;
}

extern "C" int fake_oppo_platform_uninit(void *p1, unsigned long long mask)
{
    ++g_stats.uninitCount;
    (void)p1;
    (void)mask;
    return 0;
}

extern "C" FakeOppoPlatformStats *fake_oppo_platform_stats(void)
{
    return &g_stats;
}

extern "C" void fake_oppo_platform_reset(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_initRc = 0;
}

extern "C" void fake_oppo_platform_set_process_rc(int rc)
{
    g_stats.processRc = rc;
}

extern "C" void fake_oppo_platform_set_init_rc(int rc)
{
    g_initRc = rc;
}
