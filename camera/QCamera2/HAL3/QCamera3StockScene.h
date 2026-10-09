/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef QCAMERA3_STOCK_SCENE_H
#define QCAMERA3_STOCK_SCENE_H
#include "QCamera3HdrAlgo.h"
#include <atomic>
#include <stdint.h>
namespace qcamera {
// Direct ArcSoft ASD C API. No stock HAL1 plugin or parameter records cross
// this boundary. One instance belongs to one serialized processing worker.
class QCamera3StockScene {
public:
    using Image = QCamera3HdrAlgo::Image; // NV21, with actual plane pitches.
    struct Result {
        int32_t type = -1;           // Only stock-verified semantic IDs.
        int32_t confidence = 0;      // Model score, 0..1000; not a calibrated probability.
        int32_t status = 0;          // 0 success, negative errno on failure.
        int32_t applied = 0;         // Effective tuning strength; 0 means pixels unchanged.
        uint32_t frameNumber = 0;    // Output whose processing produced this snapshot.
        uint32_t sourceFrame = 0;    // Frame actually classified by the core.
        uint32_t ageFrames = 0;
        int64_t sourceNs = 0;        // Local monotonic receipt, not sensor timestamp.
    };
    QCamera3StockScene() = default;
    ~QCamera3StockScene();
    QCamera3StockScene(const QCamera3StockScene &) = delete;
    QCamera3StockScene &operator=(const QCamera3StockScene &) = delete;
    static bool isAvailable();
    static bool knownType(int32_t type);
    static bool supports(uint32_t width, uint32_t height);
    // Transactional: input pixels remain intact after error or cancellation.
    // Cache is at most four frames / 200 ms and invalidates on content change,
    // geometry changes, backwards sequence/time, errors, or reset().
    int32_t process(const Image &image, int32_t strength, uint32_t frameNumber,
        int64_t nowNs, Result &result, const std::atomic<bool> *cancelled = nullptr);
    void reset(); // Call only after the owning worker has joined.
private:
    struct Engine;
    Engine *mEngine = nullptr;
    Result mCached;
    uint8_t mSignature[192] = {};
    uint32_t mWidth = 0, mHeight = 0;
    bool mHaveCache = false;
};
}
#endif
