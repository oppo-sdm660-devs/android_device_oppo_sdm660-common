/* SPDX-License-Identifier: BSD-3-Clause */
#include "cutils/properties.h"

#include <stdio.h>
#include <string.h>
#include <map>
#include <string>

namespace {
std::map<std::string, std::string> &props()
{
    static std::map<std::string, std::string> values;
    return values;
}
}  // namespace

extern "C" void host_prop_set(const char *key, const char *value)
{
    if (key == nullptr) return;
    props()[key] = value != nullptr ? value : "";
}

extern "C" void host_prop_clear_all(void)
{
    props().clear();
}

extern "C" int property_get(const char *key, char *value, const char *defaultValue)
{
    if (key == nullptr || value == nullptr) return 0;
    auto it = props().find(key);
    const std::string &result = it != props().end() ? it->second :
            std::string(defaultValue != nullptr ? defaultValue : "");
    snprintf(value, PROPERTY_VALUE_MAX, "%s", result.c_str());
    return int(result.size());
}

extern "C" int property_set(const char *key, const char *value)
{
    host_prop_set(key, value);
    return 0;
}
