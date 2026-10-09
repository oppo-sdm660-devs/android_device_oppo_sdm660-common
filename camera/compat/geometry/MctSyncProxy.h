/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef OPPO_CAMERA_MCT_SYNC_PROXY_H
#define OPPO_CAMERA_MCT_SYNC_PROXY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int oppo_camera_mct_register_paired_sessions(uint32_t main_session, uint32_t aux_session);
void oppo_camera_mct_unregister_paired_sessions(uint32_t main_session, uint32_t aux_session);

#ifdef __cplusplus
}
#endif
#endif
