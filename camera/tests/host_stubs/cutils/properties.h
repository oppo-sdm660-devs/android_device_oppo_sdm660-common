/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-only stand-in for Android's cutils/properties.h (no device build uses this). */
#ifndef HOST_CUTILS_PROPERTIES_H
#define HOST_CUTILS_PROPERTIES_H

#define PROPERTY_VALUE_MAX 92

#ifdef __cplusplus
extern "C" {
#endif

int property_get(const char *key, char *value, const char *default_value);
int property_set(const char *key, const char *value);

/* Test hooks: set/clear fake properties before exercising the wrapper. */
void host_prop_set(const char *key, const char *value);
void host_prop_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif
