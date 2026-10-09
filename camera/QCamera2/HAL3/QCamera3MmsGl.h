/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_MMS_GL_H
#define QCAMERA3_MMS_GL_H

#include <atomic>
#include <stdint.h>

#include "QCamera3MmsVstab.h"

namespace qcamera {

/* GPU execution unit for QCamera3MmsVstab::render(): same inputs, validation and
 * errno contract; falls back to the CPU renderer and latches off on failure. */
class QCamera3MmsGl {
public:
    using Image = QCamera3MmsVstab::Image;
    using Vertex = QCamera3MmsVstab::Vertex;

    /* Backend not selected; negative returns mean the GPU was selected but failed. */
    static constexpr int32_t kNotHandled = 1;

    struct Status {
        /* 0 = CPU reference renderer, 1 = GPU. */
        int32_t renderer;
        /* Negative errno of a GPU attempt that fell back to the CPU. */
        int32_t gpuStatus;
    };

    /* persist.vendor.camera.oppo.vstab.gpu, default 1 (0 = CPU renderer only). */
    static bool enabled();
    /* Re-reads the gate; called once per MMS session, never per frame. */
    static void refreshGate();
    /* persist.vendor.camera.oppo.vstab.gpu.probe, default 0. */
    static bool probeRequested();

    /* Builds EGL/GL state ahead of the first frame; never fatal. */
    static void prewarm();

    /* 0 = GPU drew it, kNotHandled = backend off, -ECANCELED = propagate,
     * other < 0 = CPU fallback. Destination untouched on failure. */
    static int32_t renderIfEnabled(const Image &source, const Image &destination,
            const Vertex *vertices, uint32_t columns, uint32_t rows,
            const std::atomic<bool> *cancelled);

    static Status lastStatus();

private:
    /* Static-only: the EGL context is process state. */
    QCamera3MmsGl() = delete;
    QCamera3MmsGl(const QCamera3MmsGl &) = delete;
    QCamera3MmsGl &operator=(const QCamera3MmsGl &) = delete;
};

} // namespace qcamera
#endif
