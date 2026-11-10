/* SPDX-License-Identifier: BSD-3-Clause */
#include <atomic>
#include <stdint.h>
static int mode = 0;
static std::atomic<bool> *cancelFlag = nullptr;
extern "C" void stockFilterTestMode(int value, std::atomic<bool> *flag) {
    mode = value; cancelFlag = flag;
}
extern "C" int Filter_Process(int type, const uint8_t *input, uint8_t *output,
        int width, int height, int inputPitch, int outputPitch, int cores, int flip) {
    if (type < 0 || type > 20 || input != output || width < 1 || height < 1 ||
            inputPitch != width * 4 || outputPitch != width * 4 || cores != 1 ||
            flip < 0 || flip > 2) return -100;
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            output[row * outputPitch + col * 4] ^= 0xff;
    if (mode == 1) return -5;
    if (mode == 2 && cancelFlag) cancelFlag->store(true);
    return 0;
}
