/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3MmsGl.h"
namespace qcamera {
bool QCamera3MmsGl::enabled() { return false; }
void QCamera3MmsGl::refreshGate() {}
bool QCamera3MmsGl::probeRequested() { return false; }
void QCamera3MmsGl::prewarm() {}
int32_t QCamera3MmsGl::renderIfEnabled(const Image &, const Image &, const Vertex *,
        uint32_t, uint32_t, const std::atomic<bool> *) { return kNotHandled; }
QCamera3MmsGl::Status QCamera3MmsGl::lastStatus() { return {0, 0}; }
}
