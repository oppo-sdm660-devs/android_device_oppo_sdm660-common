/*
 * Copyright (C) 2022-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include <stdlib.h>
#include <fcntl.h>

#include <unordered_map>

struct ModelInfo {
    const char* device; // ro.product.device
    const char* model;  // ro.product.model
};

const std::unordered_map<int, ModelInfo> kModelInfoMap = {
    {16051, {"R11",        "OPPO R11"}},
    {16103, {"R11Plus",    "OPPO R11 Plus"}},
    {16118, {"R11Plusk",   "OPPO R11 Plusk"}},
    
    {17011, {"R11s",       "OPPO R11s"}},
    {17021, {"R11sPlus",   "OPPO R11s Plus"}},
};

/*
 * SetProperty does not allow updating read only properties and as a result
 * does not work for our use case. Write "OverrideProperty" to do practically
 * the same thing as "SetProperty" without this restriction.
 */
void OverrideProperty(const char* name, const char* value) {
    size_t valuelen = strlen(value);

    prop_info* pi = (prop_info*)__system_property_find(name);
    if (pi != nullptr) {
        __system_property_update(pi, value, valuelen);
    } else {
        __system_property_add(name, strlen(name), value, valuelen);
    }
}

void SetupModelProperties(const ModelInfo& info) {
    struct PropPair {
        const char* key;
        const char* value;
    } props[] = {
        {"ro.product.device", info.device},
        {"ro.product.model",  info.model},
    };
    for (const auto& p : props) {
        OverrideProperty(p.key, p.value);
    }
}

int read_file(const char *fname, char *data, int max_size) {
    int fd, rc;

    if (max_size < 1)
        return 0;

    fd = open(fname, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    rc = read(fd, data, max_size - 1);
    if ((rc > 0) && (rc < max_size))
        data[rc] = '\0';
    else
        data[0] = '\0';
    close(fd);

    return 1;
}

void vendor_load_properties() {
    char const *prj_file = "/proc/oppoVersion/prjVersion";
    char prj_version[16];

    if (read_file(prj_file, prj_version, sizeof(prj_version))) {
        auto model_info = kModelInfoMap.find(atoi(prj_version));

        SetupModelProperties(model_info->second);
    }
}