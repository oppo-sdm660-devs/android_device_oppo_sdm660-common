/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_FRAME_NUMBER_MAP_H
#define QCAMERA3_FRAME_NUMBER_MAP_H

#include <cstddef>
#include <cstdint>
#include <map>

namespace qcamera {
// Active mappings outlive arbitrary subsequent preview traffic. Only retired
// mappings use the short history window needed for trailing daemon callbacks.
class QCamera3FrameNumberMap {
public:
    static constexpr uint32_t InternalOnly = UINT32_MAX;
    void insert(uint32_t internal, uint32_t framework) { entries_[internal] = {framework, true}; }
    bool get(uint32_t internal, uint32_t &framework) const {
        const auto it = entries_.find(internal);
        if (it == entries_.end()) return false;
        framework = it->second.framework;
        return true;
    }
    void retireFramework(uint32_t framework) {
        for (auto &entry : entries_)
            if (entry.second.framework == framework) entry.second.active = false;
    }
    void retireInternal(uint32_t internal) {
        const auto it = entries_.find(internal);
        if (it != entries_.end() && it->second.framework == InternalOnly) it->second.active = false;
    }
    void retireAllInternal() {
        for (auto &entry : entries_)
            if (entry.second.framework == InternalOnly) entry.second.active = false;
    }
    void purge(uint32_t next, uint32_t history) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!it->second.active && uint32_t(next - it->first) > history) it = entries_.erase(it);
            else ++it;
        }
    }
    size_t size() const { return entries_.size(); }
private:
    struct Entry { uint32_t framework; bool active; };
    std::map<uint32_t, Entry> entries_;
};
} // namespace qcamera
#endif
