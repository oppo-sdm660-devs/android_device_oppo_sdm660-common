/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
struct Vertex { float x, y, z; };
struct Frame {
    uint32_t size, revision;
    int64_t timestampNs, exposureNs;
    float zoom, gain;
    int32_t rotation;
    void *debugImage;
};
struct Output { uint32_t size, revision; Vertex *vertices; };
static_assert(sizeof(Frame) == 40, "MMS ARM32 frame");
static_assert(sizeof(Output) == 12, "MMS ARM32 output");
struct Engine { uint8_t config[36]; uint32_t columns, rows; };
int createStatus, warpStatus, addStatus, invalidConfig, invalidOutput, invalidMesh;
int created, destroyed, warped, added;
int64_t lastFrameNs;
}
extern "C" {
void fakeMmsReset() {
    createStatus = warpStatus = addStatus = invalidConfig = invalidOutput = invalidMesh = 0;
    created = destroyed = warped = added = 0;
    lastFrameNs = 0;
}
void fakeMmsSet(int key, int value) {
    switch (key) {
    case 0: createStatus = value; break;
    case 1: warpStatus = value; break;
    case 2: addStatus = value; break;
    case 3: invalidConfig = value; break;
    case 4: invalidOutput = value; break;
    case 5: invalidMesh = value; break;
    }
}
int fakeMmsGet(int key) {
    switch (key) {
    case 0: return created;
    case 1: return destroyed;
    case 2: return warped;
    case 3: return added;
    default: return -1;
    }
}
int64_t fakeMmsLastTimestamp() { return lastFrameNs; }
int32_t vStabCreateUseTextCnfgFile(void **output, const char *path, char *, int32_t *) {
    ++created;
    if (createStatus) return createStatus;
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    Engine *engine = static_cast<Engine *>(calloc(1, sizeof(Engine)));
    if (!engine) { fclose(file); return -1; }
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        char key[128];
        unsigned value;
        if (sscanf(line, " %127[^= ] = %u", key, &value) != 2) continue;
        if (!strcmp(key, "gridVrtxCntH")) engine->columns = value;
        if (!strcmp(key, "gridVrtxCntV")) engine->rows = value;
    }
    fclose(file);
    uint32_t columns = engine->columns + (invalidConfig ? 1 : 0);
    memcpy(engine->config + 0x1c, &columns, sizeof(columns));
    memcpy(engine->config + 0x20, &engine->rows, sizeof(engine->rows));
    *output = engine;
    return 0;
}
int32_t vStabDestroy(void *engine) { ++destroyed; free(engine); return 0; }
const void *vStabConfig(void *engine) { return static_cast<Engine *>(engine)->config; }
int32_t vStabAddAngularVelocity(void *, int64_t, const float *) {
    ++added;
    return addStatus;
}
int32_t vStabWarpGrid(void *handle, const Frame *frame, Output *output) {
    ++warped;
    if (frame->size != 40 || frame->revision != 2 || frame->timestampNs <= 0 ||
            frame->exposureNs <= 0 || frame->zoom < 1 || frame->gain != 1 ||
            frame->rotation != 0 || frame->debugImage || output->size != 12 ||
            output->revision != 1 || !output->vertices) abort();
    lastFrameNs = frame->timestampNs;
    if (warpStatus) return warpStatus;
    Engine *engine = static_cast<Engine *>(handle);
    for (uint32_t y = 0; y < engine->rows; ++y)
        for (uint32_t x = 0; x < engine->columns; ++x)
            output->vertices[y * engine->columns + x] = {
                2.0f * x / (engine->columns - 1) - 1.0f,
                2.0f * y / (engine->rows - 1) - 1.0f, 0.0f};
    if (invalidMesh) output->vertices[0].x = 0.0f;
    if (invalidOutput) output->revision = 0;
    return 0;
}
}
