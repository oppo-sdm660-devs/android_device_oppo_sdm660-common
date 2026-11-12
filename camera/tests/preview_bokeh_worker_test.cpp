/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3PreviewBokehWorker.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <future>

using Worker = qcamera::QCamera3PreviewBokehWorker;
using Backend = Worker::Backend;
static Backend::Calibration calibration() {
    Backend::Calibration c;
    c.source = Backend::Calibration::MODULE_OTP;
    const float focal = 400;
    const int16_t width = 64, height = 48;
    memcpy(c.data + 0xc, &focal, 4); memcpy(c.data + 0x28, &focal, 4);
    memcpy(c.data + 0x10, &width, 2); memcpy(c.data + 0x12, &height, 2);
    memcpy(c.data + 0x2c, &width, 2); memcpy(c.data + 0x2e, &height, 2);
    return c;
}
static Worker::Image image(std::vector<uint8_t> &bytes, unsigned w, unsigned h) {
    return {bytes.data(), bytes.size(), w, h, 0, size_t(w) * h, w, w, h, h / 2};
}
int main() {
    void *library = dlopen("libarcsoft_dualcam_refocus_preview.so", RTLD_NOW);
    assert(library);
    auto inits = reinterpret_cast<int (*)()>(dlsym(library, "fake_preview_inits"));
    auto processes = reinterpret_cast<int (*)()>(dlsym(library, "fake_preview_processes"));
    assert(inits && processes);
    Worker worker;
    const Backend::Config config{{11, 22}, {64, 32}, {48, 24}, {128, 64}, {96, 48}, 270, 2000000};
    assert(worker.configure(config, calibration(), 100000000) == 0);
    const int initializations = inits();
    std::vector<uint8_t> wide(64 * 48 * 3 / 2, 55), tele(32 * 24 * 3 / 2, 57), output(wide.size(), 0xac);
    for (size_t i = 64 * 48; i < wide.size(); i += 2) { wide[i] = 80; wide[i + 1] = 91; }
    std::fill(tele.begin() + 32 * 24, tele.end(), 93);
    const auto original = wide;
    Backend::Result result;
    for (unsigned i = 0; i < 6; ++i) {
        const Worker::Source main{11, 100 + i, 100000000LL + i * 33000000LL};
        const Worker::Source aux{22, 90 + i, main.timestampNs + 1000000};
        auto mainMetadata = main, auxMetadata = aux;
        mainMetadata.timestampNs += 333; auxMetadata.timestampNs += 777;
        worker.metadata(mainMetadata); worker.metadata(auxMetadata);
        assert(worker.auxiliary(aux, image(tele, 32, 24), false) == 0);
        const int status = worker.process(main, image(wide, 64, 48), true, image(output, 64, 48),
                {32, 24, 70}, result);
        if (status != (i < 5 ? -ENODATA : 0)) fprintf(stderr, "frame %u status=%d core=%d effect=%d\n",
                i, status, result.coreStatus, result.effectStatus);
        assert(status == (i < 5 ? -ENODATA : 0));
        if (i < 5) assert(output[0] == 0xac && !result.applied);
        else {
            assert(result.applied && result.source.frameNumbers[0] == 105 && result.source.frameNumbers[1] == 95);
            assert(result.source.timestampsNs[0] == mainMetadata.timestampNs &&
                    result.source.timestampsNs[1] == auxMetadata.timestampNs);
            assert(output[0] == (55 ^ 0x40));
            assert(output[64 * 48] == 80 && output[64 * 48 + 1] == 91);
        }
        assert(wide == original && inits() == initializations);
    }
    const auto previous = output;
    Worker::Source main{11, 200, 500000000}, aux{22, 199, 501000000};
    // An image timestamp without the matching sensor record cannot enter the core.
    const int count = processes();
    worker.metadata(main);
    assert(worker.auxiliary(aux, image(tele, 32, 24), false) == 0);
    assert(worker.process(main, image(wide, 64, 48), true, image(output, 64, 48), {32, 24, 70}, result) == -ETIMEDOUT);
    assert(output == previous && processes() == count);
    worker.flush();
    auto waiting = std::async(std::launch::async, [&] {
        return worker.process(main, image(wide, 64, 48), true, image(output, 64, 48), {32, 24, 70}, result);
    });
    assert(waiting.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    worker.flush();
    assert(waiting.get() == -ECANCELED && output == previous);
    // The same configured session resumes after flush without reinitialising ARC.
    worker.metadata(main); worker.metadata(aux);
    assert(worker.auxiliary(aux, image(tele, 32, 24), false) == 0);
    assert(worker.process(main, image(wide, 64, 48), true, image(output, 64, 48), {32, 24, 70}, result) == 0);
    assert(inits() == initializations);
    main.frameNumber++; aux.frameNumber++;
    main.timestampNs += 33000000; aux.timestampNs += 33000000;
    // Source callbacks may precede their metadata. The pending job must wait
    // for both exact identities while the driver can reuse its auxiliary slot.
    assert(worker.auxiliary(aux, image(tele, 32, 24), false) == 0);
    std::fill(tele.begin(), tele.end(), 0);
    auto delayed = std::async(std::launch::async, [&] {
        return worker.process(main, image(wide, 64, 48), true, image(output, 64, 48), {32, 24, 70}, result);
    });
    assert(delayed.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    worker.metadata(main); worker.metadata(aux);
    assert(delayed.get() == 0 && result.applied && wide == original);
    const auto beforeDisable = output;
    worker.resetTemporalState();
    main.frameNumber++; aux.frameNumber++;
    main.timestampNs += 33000000; aux.timestampNs += 33000000;
    std::fill(tele.begin() + 32 * 24, tele.end(), 93);
    worker.metadata(main); worker.metadata(aux);
    assert(worker.auxiliary(aux, image(tele, 32, 24), false) == 0);
    assert(worker.process(main, image(wide, 64, 48), true, image(output, 64, 48), {32, 24, 70}, result) == -ENODATA);
    assert(inits() == initializations + 1 && output == beforeDisable && !result.applied);
    worker.stop();
    assert(worker.process(main, image(wide, 64, 48), true, image(output, 64, 48), {32, 24, 70}, result) == -ECANCELED);
    dlclose(library);
    puts("preview bokeh worker pair/metadata/copy/flush tests: PASS");
}
