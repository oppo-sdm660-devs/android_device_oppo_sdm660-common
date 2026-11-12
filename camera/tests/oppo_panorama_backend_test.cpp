/*
 * Host test for the OSS DocPanorama backend prototype (pure C++, no Android).
 * Build/run: python3 camera/tests/run_oppo_panorama_tests.py
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "QCamera3OppoPanorama.h"

using namespace qcamera;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    ++g_checks; \
    const auto _va = (a); const auto _vb = (b); \
    if (_va != _vb) { \
        ++g_failures; \
        std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                static_cast<long long>(_va), static_cast<long long>(_vb)); \
    } \
} while (0)

namespace {

constexpr uint32_t kPreviewW = 64;
constexpr uint32_t kPreviewH = 48;
constexpr uint32_t kCanvasW = 128;
constexpr uint32_t kCanvasH = 48;
constexpr uint32_t kOutputW = 64;
constexpr uint32_t kOutputH = 48;
constexpr uint32_t kPanStep = 8; // px shift between consecutive frames

struct Scene {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;
};

Scene makeScene(uint32_t width, uint32_t height) {
    Scene s;
    s.width = width;
    s.height = height;
    s.y.resize(static_cast<size_t>(width) * height);
    s.uv.resize(static_cast<size_t>(width) * (height / 2));
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t v = (y * 3u + x * 7u + ((x * 13u + y * 17u) % 251u)) & 0xffu;
            s.y[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(v);
        }
    }
    for (size_t i = 0; i < s.uv.size(); i++)
        s.uv[i] = static_cast<uint8_t>((128 + static_cast<int>(i % 37)) & 0xff);
    return s;
}

// Frame = scene crop. With baseX decreasing by kPanStep per frame, the next
// frame appears shifted right by kPanStep relative to the previous one, which
// makes the image-based shift estimate +kPanStep (canvas x grows).
QCamera3OppoPanorama::Frame cropFrame(const Scene &scene, uint32_t baseX, uint32_t baseY,
        uint32_t width, uint32_t height, std::vector<uint8_t> *storage) {
    const size_t ySize = static_cast<size_t>(width) * height;
    const size_t uvSize = static_cast<size_t>(width) * (height / 2);
    storage->assign(ySize + uvSize, 0);
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t sy = y + baseY;
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t sx = x + baseX;
            (*storage)[static_cast<size_t>(y) * width + x] =
                    scene.y[static_cast<size_t>(sy) * scene.width + sx];
        }
    }
    uint8_t *uv = storage->data() + ySize;
    for (uint32_t y = 0; y < height / 2; y++) {
        for (uint32_t x = 0; x < width / 2; x++) {
            const uint32_t sx = baseX / 2 + x;
            const uint32_t sy = baseY / 2 + y;
            uv[static_cast<size_t>(y) * width + x * 2] =
                    scene.uv[static_cast<size_t>(sy) * scene.width + sx * 2];
            uv[static_cast<size_t>(y) * width + x * 2 + 1] =
                    scene.uv[static_cast<size_t>(sy) * scene.width + sx * 2 + 1];
        }
    }
    QCamera3OppoPanorama::Frame f;
    f.data = storage->data();
    f.length = storage->size();
    f.width = width;
    f.height = height;
    f.yStride = width;
    f.uvStride = width;
    return f;
}

QCamera3OppoPanorama::Config basicConfig() {
    QCamera3OppoPanorama::Config c;
    c.previewWidth = kPreviewW;
    c.previewHeight = kPreviewH;
    c.canvasWidth = kCanvasW;
    c.canvasHeight = kCanvasH;
    c.outputWidth = kOutputW;
    c.outputHeight = kOutputH;
    c.orientationDeg = 0;
    c.directionDeg = QCamera3OppoPanorama::DIR_AUTO;
    c.crop = false;
    c.maxFrames = 8;
    c.minFrames = 2;
    c.featherPx = 8;
    c.searchRadiusPx = 16;
    c.maxCanvasBytes = 64u * 1024u * 1024u;
    return c;
}

int paintPpm(const QCamera3OppoPanorama::Image &img, const char *path) {
    if (img.data == nullptr || img.width == 0 || img.height == 0)
        return -1;
    FILE *f = std::fopen(path, "wb");
    if (f == nullptr)
        return -1;
    std::fprintf(f, "P5\n%u %u\n255\n", img.width, img.height);
    std::fwrite(img.data, 1, static_cast<size_t>(img.width) * img.height, f);
    std::fclose(f);
    return 0;
}

void testStaticMapping() {
    CHECK_EQ(QCamera3OppoPanorama::stockDirectionCode(0), 0x12);
    CHECK_EQ(QCamera3OppoPanorama::stockDirectionCode(90), 0x11);
    CHECK_EQ(QCamera3OppoPanorama::stockDirectionCode(180), 0x22);
    CHECK_EQ(QCamera3OppoPanorama::stockDirectionCode(270), 0x21);
    CHECK_EQ(QCamera3OppoPanorama::stockDirectionCode(45), -1);

    uint32_t w = 0;
    uint32_t h = 0;
    CHECK(QCamera3OppoPanorama::stockAspectSize(64, 48, &w, &h));
    CHECK_EQ(w, 320u);
    CHECK_EQ(h, 240u);
    CHECK(QCamera3OppoPanorama::stockAspectSize(64, 36, &w, &h));
    CHECK_EQ(w, 640u);
    CHECK_EQ(h, 360u);
    CHECK(QCamera3OppoPanorama::stockAspectSize(64, 32, &w, &h));
    CHECK_EQ(w, 640u);
    CHECK_EQ(h, 320u);
    CHECK(QCamera3OppoPanorama::stockAspectSize(76, 36, &w, &h));
    CHECK_EQ(w, 646u);
    CHECK_EQ(h, 306u);
    CHECK(QCamera3OppoPanorama::stockAspectSize(78, 36, &w, &h));
    CHECK_EQ(w, 650u);
    CHECK_EQ(h, 300u);
    CHECK(!QCamera3OppoPanorama::stockAspectSize(50, 48, &w, &h));
}

void testValidation(const char *outDir) {
    QCamera3OppoPanorama p;
    QCamera3OppoPanorama::Config c = basicConfig();
    c.previewWidth = 0;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::E_PARAM);
    c = basicConfig();
    c.previewWidth = 63;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::E_PARAM);
    c = basicConfig();
    c.orientationDeg = 45;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::E_UNSUPPORTED);
    c = basicConfig();
    c.directionDeg = 45;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::E_UNSUPPORTED);
    c = basicConfig();
    c.maxFrames = 1;
    c.minFrames = 2;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::E_PARAM);
    c = basicConfig();
    c.canvasWidth = 0; // derives 3*preview
    c.canvasHeight = 0;
    c.outputWidth = 0;
    c.outputHeight = 0;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.canvasWidth(), kPreviewW * 3);
    CHECK_EQ(p.canvasHeight(), kPreviewH);
    CHECK_EQ(p.outputWidth(), 320u); // 4:3 stock table
    CHECK_EQ(p.outputHeight(), 240u);
    p.deinit();

    c = basicConfig();
    c.maxCanvasBytes = 1024; // too small
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::E_NOMEM);

    // addFrame before init
    Scene scene = makeScene(96, 56);
    std::vector<uint8_t> y;
    QCamera3OppoPanorama::Frame f = cropFrame(scene, 24, 0, kPreviewW, kPreviewH, &y);
    CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::E_STATE);

    CHECK_EQ(p.init(basicConfig()), QCamera3OppoPanorama::OK);
    QCamera3OppoPanorama::Frame bad = f;
    bad.data = nullptr;
    CHECK_EQ(p.addFrame(bad, nullptr), QCamera3OppoPanorama::E_PARAM);
    bad = f;
    bad.length = 10;
    CHECK_EQ(p.addFrame(bad, nullptr), QCamera3OppoPanorama::E_PARAM);
    bad = f;
    bad.width = 32;
    CHECK_EQ(p.addFrame(bad, nullptr), QCamera3OppoPanorama::E_PARAM);

    // finetune/stitch with too few frames
    CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.finetune(), QCamera3OppoPanorama::E_NOFRAME);
    std::vector<uint8_t> outBuf(p.outputSize());
    QCamera3OppoPanorama::Image img;
    img.data = outBuf.data();
    img.length = outBuf.size();
    CHECK_EQ(p.stitch(&img), QCamera3OppoPanorama::E_NOFRAME);
    p.deinit();
    (void)outDir;
}

void testHorizontalPan(const char *outDir) {
    QCamera3OppoPanorama p;
    CHECK_EQ(p.init(basicConfig()), QCamera3OppoPanorama::OK);
    Scene scene = makeScene(96, 56);
    std::vector<uint8_t> y;
    const uint32_t bases[] = {0, 8, 16, 24};
    int32_t expected = 0;
    for (uint32_t i = 0; i < 4; i++) {
        QCamera3OppoPanorama::Frame f = cropFrame(scene, bases[i], 0, kPreviewW,
                kPreviewH, &y);
        CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
        expected = static_cast<int32_t>(i * kPanStep);
        CHECK_EQ(p.currentOffset().x, expected);
        CHECK_EQ(p.currentOffset().y, 0);
    }
    CHECK_EQ(p.stats().frames, 4u);
    CHECK(p.stats().peakBytes > 0);
    const uint32_t offsetAfter = static_cast<uint32_t>(p.stats().shiftPx);
    CHECK_EQ(offsetAfter, kPanStep);

    CHECK_EQ(p.finetune(), QCamera3OppoPanorama::OK);
    std::vector<uint8_t> outBuf(p.outputSize());
    CHECK(!outBuf.empty());
    CHECK_EQ(outBuf.size(), QCamera3OppoPanorama::nv21Size(kOutputW, kOutputH, kOutputW, kOutputW));
    QCamera3OppoPanorama::Image img;
    img.data = outBuf.data();
    img.length = outBuf.size();
    CHECK_EQ(p.stitch(&img), QCamera3OppoPanorama::OK);
    CHECK_EQ(img.width, kOutputW);
    CHECK_EQ(img.height, kOutputH);
    CHECK_EQ(img.length, QCamera3OppoPanorama::nv21Size(kOutputW, kOutputH, kOutputW, kOutputW));

    // Output must contain real content, not a black/blank frame.
    uint64_t sum = 0;
    uint32_t nonzero = 0;
    for (size_t i = 0; i < static_cast<size_t>(img.width) * img.height; i++) {
        sum += outBuf[i];
        if (outBuf[i] != 0)
            nonzero++;
    }
    CHECK(nonzero > static_cast<size_t>(img.width) * img.height / 2);
    const uint32_t mean = static_cast<uint32_t>(sum / (static_cast<size_t>(img.width) * img.height));
    CHECK(mean > 20 && mean < 240);
    if (outDir != nullptr) {
        const std::string path = std::string(outDir) + "/panorama_horizontal.ppm";
        CHECK_EQ(paintPpm(img, path.c_str()), 0);
    }
    CHECK_EQ(p.stitch(&img), QCamera3OppoPanorama::OK); // idempotent enough
    p.deinit();
}

void testCropVsFullCanvas(const char *outDir) {
    Scene scene = makeScene(96, 56);
    std::vector<uint8_t> y;
    const uint32_t bases[] = {0, 8, 16, 24};

    auto run = [&](bool crop, std::vector<uint8_t> *outBuf) {
        QCamera3OppoPanorama p;
        QCamera3OppoPanorama::Config c = basicConfig();
        c.crop = crop;
        CHECK_EQ(p.init(c), QCamera3OppoPanorama::OK);
        for (uint32_t i = 0; i < 4; i++) {
            QCamera3OppoPanorama::Frame f = cropFrame(scene, bases[i], 0, kPreviewW,
                    kPreviewH, &y);
            CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
        }
        CHECK_EQ(p.finetune(), QCamera3OppoPanorama::OK);
        outBuf->assign(p.outputSize(), 0);
        QCamera3OppoPanorama::Image img;
        img.data = outBuf->data();
        img.length = outBuf->size();
        CHECK_EQ(p.stitch(&img), QCamera3OppoPanorama::OK);
        return img;
    };
    std::vector<uint8_t> cropOut;
    std::vector<uint8_t> fullOut;
    QCamera3OppoPanorama::Image imgCrop = run(true, &cropOut);
    QCamera3OppoPanorama::Image imgFull = run(false, &fullOut);
    CHECK_EQ(imgCrop.width, kOutputW);
    CHECK_EQ(imgFull.width, kOutputW);
    if (outDir != nullptr) {
        std::string base = std::string(outDir);
        CHECK_EQ(paintPpm(imgCrop, (base + "/panorama_crop.ppm").c_str()), 0);
        CHECK_EQ(paintPpm(imgFull, (base + "/panorama_full.ppm").c_str()), 0);
    }
}

void testGyroPriorOnly() {
    QCamera3OppoPanorama p;
    QCamera3OppoPanorama::Config c = basicConfig();
    c.searchRadiusPx = 0; // gyro prior only
    c.pxPerDeg = 1.0f;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::OK);
    Scene scene = makeScene(96, 56);
    std::vector<uint8_t> y;
    QCamera3OppoPanorama::Frame f = cropFrame(scene, 24, 0, kPreviewW, kPreviewH, &y);
    QCamera3OppoPanorama::GyroSample g;
    g.valid = true;
    for (uint32_t i = 0; i < 3; i++) {
        g.angleXDeg = static_cast<float>(kPanStep);
        CHECK_EQ(p.addFrame(f, &g), QCamera3OppoPanorama::OK);
        CHECK_EQ(p.currentOffset().x, static_cast<int32_t>(i * kPanStep));
    }
    CHECK_EQ(p.addGyro(g), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.currentOffset().x, 3 * int32_t(kPanStep));
    CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.currentOffset().x, 3 * int32_t(kPanStep));
    p.deinit();

    // gyro absent: falls back to the image estimate, which is zero on identical
    // frames; the frame must still be accepted.
    QCamera3OppoPanorama p2;
    CHECK_EQ(p2.init(basicConfig()), QCamera3OppoPanorama::OK);
    CHECK_EQ(p2.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    CHECK_EQ(p2.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    CHECK_EQ(p2.currentOffset().x, 0);
    CHECK_EQ(p2.finetune(), QCamera3OppoPanorama::OK);
}

void testVerticalPan() {
    QCamera3OppoPanorama p;
    QCamera3OppoPanorama::Config c = basicConfig();
    c.canvasWidth = kPreviewW;
    c.canvasHeight = 96;
    c.directionDeg = 90; // vertical, growing +y
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::OK);
    Scene scene = makeScene(64, 64);
    std::vector<uint8_t> y;
    const uint32_t bases[] = {0, 8, 16};
    for (uint32_t i = 0; i < 3; i++) {
        QCamera3OppoPanorama::Frame f = cropFrame(scene, 0, bases[i], kPreviewW,
                kPreviewH, &y);
        CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
        CHECK_EQ(p.currentOffset().y, static_cast<int32_t>(i * kPanStep));
        CHECK_EQ(p.currentOffset().x, 0);
    }
    p.deinit();
}

void testCenteredBidirectionalSweepPreservesSampledSize() {
    Scene scene = makeScene(128, 112);
    for (bool vertical : {false, true}) {
        for (int sign : {-1, 1}) {
            auto config = basicConfig();
            config.verticalSweep = vertical;
            config.canvasWidth = vertical ? kPreviewW : 3 * kPreviewW;
            config.canvasHeight = vertical ? 3 * kPreviewH : kPreviewH;
            config.centerCanvas = true;
            config.crop = true;
            config.preserveCropSize = true;
            QCamera3OppoPanorama backend;
            CHECK_EQ(backend.init(config), QCamera3OppoPanorama::OK);
            std::vector<uint8_t> storage;
            for (uint32_t n = 0; n < 4; ++n) {
                const uint32_t base = sign > 0 ? n * kPanStep : (3 - n) * kPanStep;
                const auto frame = cropFrame(scene, vertical ? 0 : base, vertical ? base : 0,
                        kPreviewW, kPreviewH, &storage);
                CHECK_EQ(backend.addFrame(frame, nullptr), QCamera3OppoPanorama::OK);
                CHECK_EQ(backend.currentOffset().x, vertical ? 0 : int(kPreviewW) + sign * int(n * kPanStep));
                CHECK_EQ(backend.currentOffset().y, vertical ? int(kPreviewH) + sign * int(n * kPanStep) : 0);
            }
            CHECK_EQ(backend.finetune(), QCamera3OppoPanorama::OK);
            std::vector<uint8_t> output(backend.outputSize());
            QCamera3OppoPanorama::Image image;
            image.data = output.data();
            image.length = output.size();
            CHECK_EQ(backend.stitch(&image), QCamera3OppoPanorama::OK);
            CHECK_EQ(image.width, kPreviewW + (vertical ? 0 : 3 * kPanStep));
            CHECK_EQ(image.height, kPreviewH + (vertical ? 3 * kPanStep : 0));
            uint32_t mismatches = 0;
            for (uint32_t y = 0; y < image.height; ++y)
                for (uint32_t x = 0; x < image.width; ++x)
                    if (std::abs(int(output[size_t(y) * image.width + x]) -
                            int(scene.y[size_t(y) * scene.width + x])) > 1) ++mismatches;
            CHECK_EQ(mismatches, 0u);
        }
    }
}

void testOrientation(const char *outDir) {
    QCamera3OppoPanorama p;
    QCamera3OppoPanorama::Config c = basicConfig();
    c.orientationDeg = 90;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::OK);
    Scene scene = makeScene(96, 56);
    std::vector<uint8_t> y;
    const uint32_t bases[] = {0, 8, 16, 24};
    for (uint32_t i = 0; i < 4; i++) {
        QCamera3OppoPanorama::Frame f = cropFrame(scene, bases[i], 0, kPreviewW,
                kPreviewH, &y);
        CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    }
    CHECK_EQ(p.finetune(), QCamera3OppoPanorama::OK);
    std::vector<uint8_t> outBuf(p.outputSize(), 0);
    QCamera3OppoPanorama::Image img;
    img.data = outBuf.data();
    img.length = outBuf.size();
    CHECK_EQ(p.stitch(&img), QCamera3OppoPanorama::OK);
    CHECK_EQ(img.width, kOutputH);
    CHECK_EQ(img.height, kOutputW);
    CHECK_EQ(img.orientationDeg, 90);
    uint32_t nonzero = 0;
    for (size_t i = 0; i < static_cast<size_t>(img.width) * img.height; i++)
        if (outBuf[i] != 0) nonzero++;
    CHECK(nonzero > static_cast<size_t>(img.width) * img.height / 2);
    if (outDir != nullptr) {
        const std::string path = std::string(outDir) + "/panorama_rot90.ppm";
        CHECK_EQ(paintPpm(img, path.c_str()), 0);
    }
    p.deinit();
}

void testLimitOverflowCancel() {
    Scene scene = makeScene(96, 56);
    std::vector<uint8_t> y;
    QCamera3OppoPanorama::Frame f = cropFrame(scene, 24, 0, kPreviewW, kPreviewH, &y);

    // maxFrames
    QCamera3OppoPanorama p;
    QCamera3OppoPanorama::Config c = basicConfig();
    c.maxFrames = 2;
    CHECK_EQ(p.init(c), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::OK);
    CHECK_EQ(p.addFrame(f, nullptr), QCamera3OppoPanorama::E_LIMIT);
    p.deinit();

    // out of canvas
    QCamera3OppoPanorama p2;
    QCamera3OppoPanorama::Config c2 = basicConfig();
    c2.canvasWidth = 80; // first frame 64 fits; +24 overflows
    CHECK_EQ(p2.init(c2), QCamera3OppoPanorama::OK);
    const uint32_t bases[] = {0, 8, 16, 24};
    int32_t rc = QCamera3OppoPanorama::OK;
    for (uint32_t i = 0; i < 4 && rc == QCamera3OppoPanorama::OK; i++) {
        QCamera3OppoPanorama::Frame ff = cropFrame(scene, bases[i], 0, kPreviewW,
                kPreviewH, &y);
        rc = p2.addFrame(ff, nullptr);
    }
    CHECK_EQ(rc, QCamera3OppoPanorama::E_OVERFLOW);
    CHECK(p2.stats().rejected >= 1);
    p2.deinit();

    // cancel
    QCamera3OppoPanorama p3;
    CHECK_EQ(p3.init(basicConfig()), QCamera3OppoPanorama::OK);
    p3.cancel();
    CHECK_EQ(p3.state(), QCamera3OppoPanorama::State::Canceled);
    CHECK_EQ(p3.addFrame(f, nullptr), QCamera3OppoPanorama::E_CANCELED);
    QCamera3OppoPanorama::Image img;
    std::vector<uint8_t> buf(16);
    img.data = buf.data();
    img.length = buf.size();
    CHECK_EQ(p3.stitch(&img), QCamera3OppoPanorama::E_CANCELED);
}

} // namespace

int main(int argc, char **argv) {
    const char *outDir = (argc > 1) ? argv[1] : nullptr;
    testStaticMapping();
    testValidation(outDir);
    testHorizontalPan(outDir);
    testCropVsFullCanvas(outDir);
    testGyroPriorOnly();
    testVerticalPan();
    testCenteredBidirectionalSweepPreservesSampledSize();
    testOrientation(outDir);
    testLimitOverflowCancel();

    if (g_failures != 0) {
        std::printf("oppo panorama backend: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("oppo panorama backend: %d checks passed\n", g_checks);
    return 0;
}
