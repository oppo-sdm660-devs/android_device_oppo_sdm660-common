/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-only stand-in for the stack logging header (no device build uses this). */
#ifndef HOST_MM_CAMERA_DBG_H
#define HOST_MM_CAMERA_DBG_H

#ifdef __cplusplus
extern "C" {
#endif

void host_stub_log(const char *level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#define LOGD(...) host_stub_log("D", __VA_ARGS__)
#define LOGI(...) host_stub_log("I", __VA_ARGS__)
#define LOGW(...) host_stub_log("W", __VA_ARGS__)
#define LOGE(...) host_stub_log("E", __VA_ARGS__)

#endif
