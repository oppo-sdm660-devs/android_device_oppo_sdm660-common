/* SPDX-License-Identifier: BSD-3-Clause */
// Host fake of the ARC_BS_IMAGE_* exports used by QCamera3FrontBeauty.
// It validates the adapter's call sequence and image layout only; it does not
// execute any ArcSoft algorithm.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern "C" {

struct FakeArcImage {
    uint32_t format;
    int32_t width, height;
    uint8_t *planes[4];
    int32_t pitches[4];
};

static int32_t g_failInit;
static int32_t g_failProcess;
static int32_t g_failUninit;
static int32_t g_initCalls;
static int32_t g_uninitCalls;
static int32_t g_processCalls;
static int32_t g_featureCalls;
static int32_t g_featureIds[32];
static int32_t g_featureLevels[32];
static int32_t g_processSameImage;
static int32_t g_processFaceArgNull;
static int32_t g_processArg5Null;
static uint32_t g_processFormat;
static int32_t g_processWidth;
static int32_t g_processHeight;
static int32_t g_processPitches[4];
static int32_t g_processPlanesNonNull;
static uint8_t g_chromaV[4096];
static uint8_t g_chromaU[4096];
static int32_t g_chromaStride;
static int32_t g_chromaRows;
static int32_t g_tamperDescriptor;
static uint8_t *g_processY;
static int32_t g_processYSamples;
static int32_t g_initArgument;

void fake_reset(void) {
    g_failInit = g_failProcess = g_failUninit = 0;
    g_initCalls = g_uninitCalls = g_processCalls = g_featureCalls = 0;
    memset(g_featureIds, 0, sizeof(g_featureIds));
    memset(g_featureLevels, 0, sizeof(g_featureLevels));
    g_processSameImage = g_processFaceArgNull = g_processArg5Null = 0;
    g_processFormat = 0;
    g_processWidth = g_processHeight = 0;
    memset(g_processPitches, 0, sizeof(g_processPitches));
    g_processPlanesNonNull = 0;
    memset(g_chromaV, 0, sizeof(g_chromaV));
    memset(g_chromaU, 0, sizeof(g_chromaU));
    g_chromaStride = g_chromaRows = 0;
    g_tamperDescriptor = 0;
    g_processY = nullptr;
    g_processYSamples = 0;
    g_initArgument = -1;
}
void fake_fail_init(int32_t enable) { g_failInit = enable; }
void fake_fail_process(int32_t enable) { g_failProcess = enable; }
void fake_fail_uninit(int32_t enable) { g_failUninit = enable; }
void fake_tamper_descriptor(int32_t enable) { g_tamperDescriptor = enable; }
int32_t fake_init_calls(void) { return g_initCalls; }
int32_t fake_uninit_calls(void) { return g_uninitCalls; }
int32_t fake_process_calls(void) { return g_processCalls; }
int32_t fake_feature_calls(void) { return g_featureCalls; }
int32_t fake_feature_id(int32_t index) { return g_featureIds[index]; }
int32_t fake_feature_level(int32_t index) { return g_featureLevels[index]; }
int32_t fake_same_image(void) { return g_processSameImage; }
int32_t fake_face_arg_null(void) { return g_processFaceArgNull; }
int32_t fake_arg5_null(void) { return g_processArg5Null; }
uint32_t fake_format(void) { return g_processFormat; }
int32_t fake_width(void) { return g_processWidth; }
int32_t fake_height(void) { return g_processHeight; }
int32_t fake_pitch(int32_t plane) { return g_processPitches[plane]; }
int32_t fake_planes_non_null(void) { return g_processPlanesNonNull; }
uint8_t fake_chroma_v(int32_t offset) { return g_chromaV[offset]; }
uint8_t fake_chroma_u(int32_t offset) { return g_chromaU[offset]; }
int32_t fake_chroma_stride(void) { return g_chromaStride; }
int32_t fake_chroma_rows(void) { return g_chromaRows; }
int32_t fake_init_argument(void) { return g_initArgument; }

void *ARC_BS_IMAGE_Init(void) {
    ++g_initCalls;
    if (g_failInit) return nullptr;
    return reinterpret_cast<void *>(0x1234);
}

int32_t ARC_BS_IMAGE_UnInit(void *handle) {
    ++g_uninitCalls;
    (void)handle;
    return g_failUninit;
}

void *ARC_BS_IMAGE_SetFeatureLevel(void *handle, int32_t featureId, int32_t level) {
    (void)handle;
    if (g_featureCalls < 32) {
        g_featureIds[g_featureCalls] = featureId;
        g_featureLevels[g_featureCalls] = level;
    }
    ++g_featureCalls;
    return nullptr;
}

int32_t ARC_BS_IMAGE_Process(void *handle, FakeArcImage *in, FakeArcImage *out,
        void *faceArg, void *arg5) {
    (void)handle;
    ++g_processCalls;
    g_processSameImage = in == out;
    g_processFaceArgNull = faceArg == nullptr;
    g_processArg5Null = arg5 == nullptr;
    g_processFormat = in ? in->format : 0;
    g_processWidth = in ? in->width : 0;
    g_processHeight = in ? in->height : 0;
    g_processPlanesNonNull = in ? ((in->planes[0] ? 1 : 0) + (in->planes[1] ? 1 : 0) +
            (in->planes[2] ? 1 : 0)) : 0;
    if (in) {
        for (int i = 0; i < 4; ++i) g_processPitches[i] = in->pitches[i];
        // Snapshot the planar chroma the adapter built from NV21.
        const int32_t rows = in->height / 2;
        const int32_t stride = in->pitches[1];
        g_chromaRows = rows;
        g_chromaStride = stride;
        if (in->planes[1] && in->planes[2] && stride > 0 && rows > 0 &&
                stride * rows <= (int32_t)sizeof(g_chromaV)) {
            memcpy(g_chromaV, in->planes[1], size_t(stride) * rows);
            memcpy(g_chromaU, in->planes[2], size_t(stride) * rows);
        }
        // In-place processing: mutate the luma plane, keep chroma untouched.
        g_processY = in->planes[0];
        g_processYSamples = in->planes[0] ? in->width : 0;
        if (in->planes[0])
            for (int32_t row = 0; row < in->height; ++row)
                memset(in->planes[0] + size_t(row) * in->pitches[0], 0xAA, in->width);
        if (g_tamperDescriptor) in->planes[0] = in->planes[0] + 4;
    }
    return g_failProcess ? -5 : 0;
}

void *ARCRLP_Init(int32_t argument) {
    g_initArgument = argument;
    return ARC_BS_IMAGE_Init();
}
int32_t ARCRLP_UnInit(void *handle) { return ARC_BS_IMAGE_UnInit(handle); }
void ARCRLP_SetFeatureLevel(void *handle, int32_t featureId, int32_t level) {
    ARC_BS_IMAGE_SetFeatureLevel(handle, featureId, level);
}
int32_t ARCRLP_Process(void *handle, FakeArcImage *in, FakeArcImage *out) {
    return ARC_BS_IMAGE_Process(handle, in, out, nullptr, nullptr);
}

} // extern "C"
