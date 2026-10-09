/* SPDX-License-Identifier: BSD-3-Clause */
#define LOG_TAG "VSTAB_GL"

#include "QCamera3MmsGl.h"
#include "QCamera3MmsGlMesh.h"

#include <cutils/properties.h>
#include <errno.h>
#include <log/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <system/graphics.h>
#include <ui/GraphicBuffer.h>

namespace qcamera {
namespace {

/* Khronos constants missing from older platform EGL/GLES headers. */
#ifndef EGL_CONTEXT_CLIENT_VERSION
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#endif
#ifndef EGL_OPENGL_ES2_BIT
#define EGL_OPENGL_ES2_BIT 0x0004
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

using android::GraphicBuffer;
using android::sp;

constexpr uint32_t kMaxGrid = 65;
constexpr size_t kMemoryLimit = 128U * 1024U * 1024U;
constexpr int32_t kMaxFailureLogs = 5;

/* Flexible YUV; same numeric value as AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420. */
constexpr int32_t kYuvFormat = HAL_PIXEL_FORMAT_YCbCr_420_888;

constexpr uint64_t kInputUsage = GraphicBuffer::USAGE_HW_TEXTURE |
        GraphicBuffer::USAGE_SW_WRITE_OFTEN | GraphicBuffer::USAGE_SW_READ_OFTEN;
constexpr uint64_t kOutputUsage = GraphicBuffer::USAGE_HW_RENDER |
        GraphicBuffer::USAGE_SW_READ_OFTEN | GraphicBuffer::USAGE_SW_WRITE_OFTEN;

// Verbatim from libmms_warper_vstab.so .rodata; do not edit.
static const char kVertexShader[] = R"VSTAB(#version 300 es
in  vec3 aPosition;
in  vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
   gl_Position = vec4(aPosition, 1.0);
   vTexCoord   = aTexCoord;
}
)VSTAB";
static_assert(sizeof(kVertexShader) == 160, "stock vertex shader must stay verbatim");

// Verbatim from libmms_warper_vstab.so .rodata; YUV path, no RGB conversion.
static const char kFragmentShader[] = R"VSTAB(#version 300 es
#extension GL_EXT_YUV_target : require
#extension GL_OES_EGL_image_external : require
precision mediump float;
in vec2 vTexCoord;
uniform __samplerExternal2DY2YEXT uSampler;
layout (yuv) out vec4 FragColor;
void main() {
   FragColor = vec4(texture(uSampler, vTexCoord).rgb, 1.0);
}
)VSTAB";
static_assert(sizeof(kFragmentShader) == 300, "stock fragment shader must stay verbatim");

using ImageHandle = void *;
using CreateImageFn = ImageHandle (EGLAPIENTRY *)(EGLDisplay, EGLContext, EGLenum,
        EGLClientBuffer, const EGLint *);
using DestroyImageFn = EGLBoolean (EGLAPIENTRY *)(EGLDisplay, ImageHandle);
using TargetTextureFn = void (GL_APIENTRY *)(GLenum, ImageHandle);
using TargetRenderbufferFn = void (GL_APIENTRY *)(GLenum, ImageHandle);

template <typename T> T loadEntry(const char *name) {
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

/* Process-wide backend state; renderIfEnabled() serialises on lock. */
struct State {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    bool initialized = false;
    bool ready = false;
    bool disabled = false;
    int32_t consecutiveFailures = 0;
    int32_t initError = 0;
    QCamera3MmsGl::Status status = {0, 0};

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLint major = 0;
    EGLint minor = 0;

    CreateImageFn createImage = nullptr;
    DestroyImageFn destroyImage = nullptr;
    TargetTextureFn imageTargetTexture = nullptr;
    TargetRenderbufferFn imageTargetRenderbuffer = nullptr;

    GLuint program = 0;
    GLuint framebuffer = 0;
    GLint sampler = -1;

    sp<GraphicBuffer> input;
    sp<GraphicBuffer> output;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    ImageHandle inputImage = nullptr;
    ImageHandle outputImage = nullptr;
    GLuint inputTexture = 0;
    GLuint outputRenderbuffer = 0;

    uint16_t *indices = nullptr;
    size_t indexCount = 0;
    float *uv = nullptr;
    uint32_t columns = 0;
    uint32_t rows = 0;

    char glVersion[96] = {};
    char glRenderer[96] = {};
    int32_t failuresLogged = 0;
};

State gState;

bool readGate(const char *name, const char *defaultValue = "0") {
    char value[PROPERTY_VALUE_MAX] = {};
    property_get(name, value, defaultValue);
    if (value[0] == '\0') return false;
    return atoi(value) != 0;
}

/* Cached gate: -1 unread, 0 CPU reference renderer, 1 GPU renderer. */
std::atomic<int32_t> gGate{-1};
std::atomic<int32_t> gProbe{-1};

bool validImage(const QCamera3MmsVstab::Image &image) {
    return image.width <= 8192 && image.height <= 8192 &&
            QCamera3HdrAlgo::validImage(image);
}

bool isCancelled(const std::atomic<bool> *flag) {
    return flag && flag->load();
}

void copyGlString(char *out, size_t size, GLenum name) {
    const GLubyte *value = glGetString(name);
    if (!out || !size) return;
    if (!value) {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "%s", reinterpret_cast<const char *>(value));
}

bool hasGlExtension(const char *name) {
    const char *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    if (!extensions || !name || !name[0]) return false;
    const size_t length = strlen(name);
    for (const char *hit = extensions; (hit = strstr(hit, name)) != nullptr; hit += length) {
        const char before = (hit == extensions) ? ' ' : hit[-1];
        const char after = hit[length];
        if ((before == ' ' || before == '\t') && (after == ' ' || after == '\t' || after == '\0'))
            return true;
    }
    return false;
}

GLenum takeGlError() {
    GLenum first = GL_NO_ERROR;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError())
        if (first == GL_NO_ERROR) first = error;
    return first;
}

GLuint compileShader(GLenum type, const char *source, const char *label) {
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        ALOGE("glCreateShader(%s) failed", label);
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        GLint length = 0;
        char log[512] = {};
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        if (length > 0) glGetShaderInfoLog(shader, GLsizei(sizeof(log)), nullptr, log);
        ALOGE("stock %s shader did not compile: %s", label, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint buildProgram() {
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader, "vertex");
    if (!vertex) return 0;
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader, "fragment");
    if (!fragment) {
        glDeleteShader(vertex);
        return 0;
    }
    const GLuint program = glCreateProgram();
    if (!program) {
        ALOGE("glCreateProgram failed");
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return 0;
    }
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    /* Same slots as the stock warper: 0 = position, 1 = texture coordinate. */
    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aTexCoord");
    glLinkProgram(program);
    glDetachShader(program, vertex);
    glDetachShader(program, fragment);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        char log[512] = {};
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        if (length > 0) glGetProgramInfoLog(program, GLsizei(sizeof(log)), nullptr, log);
        ALOGE("stock program did not link: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void releaseBuffersLocked() {
    if (gState.inputTexture) {
        glDeleteTextures(1, &gState.inputTexture);
        gState.inputTexture = 0;
    }
    if (gState.outputRenderbuffer) {
        glDeleteRenderbuffers(1, &gState.outputRenderbuffer);
        gState.outputRenderbuffer = 0;
    }
    if (gState.inputImage && gState.destroyImage) {
        gState.destroyImage(gState.display, gState.inputImage);
    }
    gState.inputImage = nullptr;
    if (gState.outputImage && gState.destroyImage) {
        gState.destroyImage(gState.display, gState.outputImage);
    }
    gState.outputImage = nullptr;
    gState.input.clear();
    gState.output.clear();
    gState.inputWidth = 0;
    gState.inputHeight = 0;
    gState.outputWidth = 0;
    gState.outputHeight = 0;
}

/* Init-failure cleanup only; GL objects die with the context. */
void abandonLocked() {
    if (gState.display != EGL_NO_DISPLAY && gState.context != EGL_NO_CONTEXT)
        eglMakeCurrent(gState.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gState.context != EGL_NO_CONTEXT) {
        eglDestroyContext(gState.display, gState.context);
        gState.context = EGL_NO_CONTEXT;
    }
    if (gState.surface != EGL_NO_SURFACE) {
        eglDestroySurface(gState.display, gState.surface);
        gState.surface = EGL_NO_SURFACE;
    }
    if (gState.display != EGL_NO_DISPLAY) {
        eglTerminate(gState.display);
        gState.display = EGL_NO_DISPLAY;
    }
    gState.config = nullptr;
    gState.program = 0;
    gState.framebuffer = 0;
    gState.sampler = -1;
    free(gState.indices);
    gState.indices = nullptr;
    free(gState.uv);
    gState.uv = nullptr;
    gState.indexCount = 0;
    gState.columns = 0;
    gState.rows = 0;
    gState.ready = false;
    gState.initialized = true;
}

/* Stock sequence: 8/8/8 config, 1x1 pbuffer, ES2 context.
 * Both shaders declare #version 300 es; this driver accepts that on an ES2 context. */
int32_t initializeLocked() {
    if (gState.initialized) return gState.ready ? 0 : gState.initError;
    gState.initialized = true;

    gState.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gState.display == EGL_NO_DISPLAY) {
        gState.initError = -ENODEV;
        ALOGE("eglGetDisplay failed");
        return gState.initError;
    }
    if (!eglInitialize(gState.display, &gState.major, &gState.minor)) {
        gState.initError = -ENODEV;
        ALOGE("eglInitialize failed (0x%04x)", unsigned(eglGetError()));
        abandonLocked();
        return gState.initError;
    }
    const EGLint configAttributes[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLint configCount = 0;
    if (!eglChooseConfig(gState.display, configAttributes, &gState.config, 1, &configCount) ||
            configCount < 1) {
        gState.initError = -ENODEV;
        ALOGE("eglChooseConfig failed (0x%04x, count %d)", unsigned(eglGetError()), configCount);
        abandonLocked();
        return gState.initError;
    }
    const EGLint surfaceAttributes[] = { EGL_HEIGHT, 1, EGL_WIDTH, 1, EGL_NONE };
    gState.surface = eglCreatePbufferSurface(gState.display, gState.config, surfaceAttributes);
    if (gState.surface == EGL_NO_SURFACE) {
        gState.initError = -ENODEV;
        ALOGE("eglCreatePbufferSurface failed (0x%04x)", unsigned(eglGetError()));
        abandonLocked();
        return gState.initError;
    }
    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    gState.context = eglCreateContext(gState.display, gState.config, EGL_NO_CONTEXT,
            contextAttributes);
    if (gState.context == EGL_NO_CONTEXT) {
        gState.initError = -ENODEV;
        ALOGE("eglCreateContext failed (0x%04x)", unsigned(eglGetError()));
        abandonLocked();
        return gState.initError;
    }
    if (!eglMakeCurrent(gState.display, gState.surface, gState.surface, gState.context)) {
        gState.initError = -ENODEV;
        ALOGE("eglMakeCurrent failed (0x%04x)", unsigned(eglGetError()));
        abandonLocked();
        return gState.initError;
    }

    copyGlString(gState.glVersion, sizeof(gState.glVersion), GL_VERSION);
    copyGlString(gState.glRenderer, sizeof(gState.glRenderer), GL_RENDERER);
    char languageVersion[96] = {};
    copyGlString(languageVersion, sizeof(languageVersion), GL_SHADING_LANGUAGE_VERSION);
    ALOGI("EGL %d.%d on %s", gState.major, gState.minor,
            gState.glRenderer[0] ? gState.glRenderer : "unknown");
    ALOGI("GL_VERSION %s", gState.glVersion[0] ? gState.glVersion : "unknown");
    ALOGI("GLSL %s", languageVersion[0] ? languageVersion : "unknown");
    if (!hasGlExtension("GL_EXT_YUV_target") ||
            !hasGlExtension("GL_OES_EGL_image_external")) {
        gState.initError = -ENOSYS;
        ALOGE("driver is missing GL_EXT_YUV_target or GL_OES_EGL_image_external");
        abandonLocked();
        return gState.initError;
    }

    gState.createImage = loadEntry<CreateImageFn>("eglCreateImageKHR");
    gState.destroyImage = loadEntry<DestroyImageFn>("eglDestroyImageKHR");
    gState.imageTargetTexture = loadEntry<TargetTextureFn>("glEGLImageTargetTexture2DOES");
    gState.imageTargetRenderbuffer =
            loadEntry<TargetRenderbufferFn>("glEGLImageTargetRenderbufferStorageOES");
    if (!gState.createImage || !gState.destroyImage || !gState.imageTargetTexture ||
            !gState.imageTargetRenderbuffer) {
        gState.initError = -ENOSYS;
        ALOGE("EGL/GLES entry points missing (create=%d destroy=%d texture=%d renderbuffer=%d)",
                int(gState.createImage != nullptr), int(gState.destroyImage != nullptr),
                int(gState.imageTargetTexture != nullptr),
                int(gState.imageTargetRenderbuffer != nullptr));
        abandonLocked();
        return gState.initError;
    }

    gState.program = buildProgram();
    if (!gState.program) {
        gState.initError = -EIO;
        abandonLocked();
        return gState.initError;
    }
    gState.sampler = glGetUniformLocation(gState.program, "uSampler");
    glGenFramebuffers(1, &gState.framebuffer);
    GLenum error = takeGlError();
    if (!gState.framebuffer || error != GL_NO_ERROR) {
        gState.initError = -EIO;
        ALOGE("GL setup failed (fbo %u, error 0x%04x)", gState.framebuffer, error);
        abandonLocked();
        return gState.initError;
    }
    gState.ready = true;
    ALOGI("GPU execution unit ready (%s)", gState.glVersion[0] ? gState.glVersion : "GLES");
    if (gState.sampler < 0) ALOGW("uSampler is at no location; the stock shader may not use it");
    eglMakeCurrent(gState.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return 0;
}

int32_t ensureInitializeLocked() {
    if (gState.ready) return 0;
    return initializeLocked();
}

/* Regular UV grid plus the stock strip topology, built once per grid size. */
int32_t ensureMeshLocked(uint32_t columns, uint32_t rows) {
    if (gState.indices && gState.uv && gState.columns == columns && gState.rows == rows) return 0;
    free(gState.indices);
    gState.indices = nullptr;
    free(gState.uv);
    gState.uv = nullptr;
    gState.indexCount = 0;
    gState.columns = 0;
    gState.rows = 0;

    const size_t count = mmsgl::stripIndexCount(columns, rows);
    const size_t vertices = size_t(columns) * rows;
    if (count == 0 || vertices > 0x10000u) return -EINVAL;
    uint16_t *indices = static_cast<uint16_t *>(malloc(count * sizeof(uint16_t)));
    float *uv = static_cast<float *>(malloc(vertices * 2 * sizeof(float)));
    uint8_t *scratch = static_cast<uint8_t *>(malloc(vertices));
    int32_t status = 0;
    if (!indices || !uv || !scratch) status = -ENOMEM;
    if (!status && mmsgl::selfCheck(columns, rows, indices, scratch) != 0) {
        ALOGE("stock strip topology self-check failed for a %ux%u grid", columns, rows);
        status = -EPROTO;
    }
    free(scratch);
    if (status) {
        free(indices);
        free(uv);
        return status;
    }
    mmsgl::buildUvGrid(uv, columns, rows);
    gState.indices = indices;
    gState.uv = uv;
    gState.indexCount = count;
    gState.columns = columns;
    gState.rows = rows;
    ALOGI("stock strip topology for a %ux%u grid: %zu elements", columns, rows, count);
    return 0;
}

/* Input is private heap, not an EGLImage source: stage through gralloc buffers. */
int32_t ensureBuffersLocked(uint32_t inputW, uint32_t inputH,
        uint32_t outputW, uint32_t outputH) {
    if (gState.input != nullptr && gState.output != nullptr &&
            gState.inputWidth == inputW && gState.inputHeight == inputH &&
            gState.outputWidth == outputW && gState.outputHeight == outputH) return 0;
    releaseBuffersLocked();

    gState.input = new GraphicBuffer(inputW, inputH, kYuvFormat, 1, kInputUsage,
            "QCamera3MmsGlIn");
    gState.output = new GraphicBuffer(outputW, outputH, kYuvFormat, 1, kOutputUsage,
            "QCamera3MmsGlOut");
    if (gState.input == nullptr || gState.output == nullptr ||
            gState.input->initCheck() != 0 || gState.output->initCheck() != 0) {
        ALOGE("gralloc staging allocation failed for %ux%u -> %ux%u (%d/%d)", inputW, inputH,
                outputW, outputH, int(gState.input != nullptr && gState.input->initCheck() != 0),
                int(gState.output != nullptr && gState.output->initCheck() != 0));
        releaseBuffersLocked();
        return -ENOMEM;
    }

    const EGLint imageAttributes[] = { EGL_IMAGE_PRESERVED_KHR, 1, EGL_NONE };
    gState.inputImage = gState.createImage(gState.display, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID,
            reinterpret_cast<EGLClientBuffer>(gState.input->getNativeBuffer()),
            imageAttributes);
    if (!gState.inputImage) {
        ALOGE("input eglCreateImageKHR failed (0x%04x)", unsigned(eglGetError()));
        releaseBuffersLocked();
        return -EIO;
    }
    gState.outputImage = gState.createImage(gState.display, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID,
            reinterpret_cast<EGLClientBuffer>(gState.output->getNativeBuffer()),
            imageAttributes);
    if (!gState.outputImage) {
        ALOGE("output eglCreateImageKHR failed (0x%04x)", unsigned(eglGetError()));
        releaseBuffersLocked();
        return -EIO;
    }

    glGenTextures(1, &gState.inputTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, gState.inputTexture);
    gState.imageTargetTexture(GL_TEXTURE_EXTERNAL_OES, gState.inputImage);
    /* Same filtering and edge mode as the stock warper. */
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    glGenRenderbuffers(1, &gState.outputRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, gState.outputRenderbuffer);
    gState.imageTargetRenderbuffer(GL_RENDERBUFFER, gState.outputImage);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, gState.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
            gState.outputRenderbuffer);
    const GLenum complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const GLenum error = takeGlError();
    if (complete != GL_FRAMEBUFFER_COMPLETE) {
        ALOGE("YUV render target is not complete (0x%04x)", complete);
        releaseBuffersLocked();
        return -EIO;
    }
    if (error != GL_NO_ERROR) {
        ALOGE("staging GL setup failed (0x%04x)", error);
        releaseBuffersLocked();
        return -EIO;
    }
    gState.inputWidth = inputW;
    gState.inputHeight = inputH;
    gState.outputWidth = outputW;
    gState.outputHeight = outputH;
    ALOGI("staging buffers ready: %ux%u in, %ux%u out", inputW, inputH, outputW, outputH);
    return 0;
}

/* Packed NV12 -> flexible YUV; plane offsets come from lockYCbCr, not stride*height. */
int32_t uploadLocked(const QCamera3MmsVstab::Image &source) {
    android_ycbcr layout = {};
    if (gState.input->lockYCbCr(GRALLOC_USAGE_SW_WRITE_OFTEN, &layout) != 0) {
        if (gState.failuresLogged < kMaxFailureLogs)
            ALOGE("input staging lockYCbCr failed");
        return -EIO;
    }
    int32_t status = 0;
    /* Row must cover the last chroma pair: width/2 * chroma_step bytes. */
    if (!layout.y || !layout.cb || !layout.cr || !layout.chroma_step || layout.chroma_step > 2u ||
            layout.ystride < source.width ||
            layout.cstride < (source.width / 2) * layout.chroma_step) {
        status = -EINVAL;
    } else {
        for (uint32_t row = 0; row < source.height; ++row)
            memcpy(static_cast<uint8_t *>(layout.y) + row * layout.ystride,
                    source.data + source.yOffset + row * source.yStride, source.width);
        for (uint32_t row = 0; row < source.height / 2; ++row)
            for (uint32_t column = 0; column < source.width / 2; ++column) {
                static_cast<uint8_t *>(layout.cb)[row * layout.cstride + column * layout.chroma_step] =
                        source.data[source.uvOffset + row * source.uvStride + column * 2];
                static_cast<uint8_t *>(layout.cr)[row * layout.cstride + column * layout.chroma_step] =
                        source.data[source.uvOffset + row * source.uvStride + column * 2 + 1];
            }
    }
    const int32_t unlocked = gState.input->unlock();
    return status ? status : unlocked;
}

/* Flexible YUV -> packed NV12; destination untouched when the lock fails. */
int32_t readbackLocked(const QCamera3MmsVstab::Image &destination) {
    android_ycbcr layout = {};
    if (gState.output->lockYCbCr(GRALLOC_USAGE_SW_READ_OFTEN, &layout) != 0) {
        if (gState.failuresLogged < kMaxFailureLogs)
            ALOGE("output staging lockYCbCr failed");
        return -EIO;
    }
    int32_t status = 0;
    if (!layout.y || !layout.cb || !layout.cr || !layout.chroma_step || layout.chroma_step > 2u ||
            layout.ystride < destination.width ||
            layout.cstride < (destination.width / 2) * layout.chroma_step) {
        status = -EINVAL;
    } else {
        for (uint32_t row = 0; row < destination.height; ++row)
            memcpy(destination.data + destination.yOffset + row * destination.yStride,
                    static_cast<uint8_t *>(layout.y) + row * layout.ystride, destination.width);
        for (uint32_t row = 0; row < destination.height / 2; ++row)
            for (uint32_t column = 0; column < destination.width / 2; ++column) {
                destination.data[destination.uvOffset + row * destination.uvStride + column * 2] =
                        static_cast<uint8_t *>(layout.cb)[row * layout.cstride +
                                column * layout.chroma_step];
                destination.data[destination.uvOffset + row * destination.uvStride + column * 2 + 1] =
                        static_cast<uint8_t *>(layout.cr)[row * layout.cstride +
                                column * layout.chroma_step];
            }
    }
    const int32_t unlocked = gState.output->unlock();
    return status ? status : unlocked;
}

/* Stock draw; viewport is the destination size and no vertical flip is applied. */
int32_t drawLocked(const QCamera3MmsVstab::Vertex *vertices,
        const QCamera3MmsVstab::Image &destination) {
    glBindFramebuffer(GL_FRAMEBUFFER, gState.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
            gState.outputRenderbuffer);
    GLenum complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (complete != GL_FRAMEBUFFER_COMPLETE) {
        if (gState.failuresLogged < kMaxFailureLogs)
            ALOGE("YUV render target became incomplete (0x%04x)", complete);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -EIO;
    }
    /* Stock clear colour. */
    glClearColor(0.5f, 0.2f, 0.8f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, GLsizei(destination.width), GLsizei(destination.height));
    glUseProgram(gState.program);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, gState.inputTexture);
    glUniform1i(gState.sampler, 0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, gState.uv);
    glDrawElements(GL_TRIANGLE_STRIP, GLsizei(gState.indexCount), GL_UNSIGNED_SHORT,
            gState.indices);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glUseProgram(0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* CPU readback follows, so the draw must be resolved here. */
    glFinish();
    const GLenum error = takeGlError();
    if (error != GL_NO_ERROR) {
        if (gState.failuresLogged < kMaxFailureLogs) ALOGE("draw failed (0x%04x)", error);
        return -EIO;
    }
    return 0;
}

void setStatusLocked(int32_t renderer, int32_t gpuStatus) {
    gState.status.renderer = renderer;
    gState.status.gpuStatus = gpuStatus;
}

int32_t failLocked(int32_t status) {
    setStatusLocked(0, status);
    if (gState.failuresLogged < kMaxFailureLogs) {
        ++gState.failuresLogged;
        if (gState.failuresLogged == kMaxFailureLogs)
            ALOGE("GPU frame failed (%d); further failures stay silent and are reported "
                    "through vstabGpuStatus only", status);
        else
            ALOGE("GPU frame failed (%d); using the CPU reference renderer", status);
    }
    return status;
}

} // namespace

bool QCamera3MmsGl::enabled() {
    int32_t value = gGate.load(std::memory_order_relaxed);
    if (value < 0) {
        value = readGate("persist.vendor.camera.oppo.vstab.gpu", "1") ? 1 : 0;
        gGate.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

void QCamera3MmsGl::refreshGate() {
    const int32_t value = readGate("persist.vendor.camera.oppo.vstab.gpu", "1") ? 1 : 0;
    const int32_t previous = gGate.exchange(value, std::memory_order_relaxed);
    if (previous >= 0 && previous != value)
        ALOGI("GPU execution unit gate changed from %d to %d for this session", previous, value);
    gProbe.store(readGate("persist.vendor.camera.oppo.vstab.gpu.probe") ? 1 : 0,
            std::memory_order_relaxed);
    /* A new session re-arms a latched-off backend. */
    pthread_mutex_lock(&gState.lock);
    if (gState.disabled) ALOGI("GPU execution unit re-armed for this session");
    gState.disabled = false;
    gState.consecutiveFailures = 0;
    gState.failuresLogged = 0;
    pthread_mutex_unlock(&gState.lock);
}

bool QCamera3MmsGl::probeRequested() {
    int32_t value = gProbe.load(std::memory_order_relaxed);
    if (value < 0) {
        value = readGate("persist.vendor.camera.oppo.vstab.gpu.probe") ? 1 : 0;
        gProbe.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

void QCamera3MmsGl::prewarm() {
    /* Never build an EGL context for a session that will not use it. */
    if (!enabled() && !probeRequested()) return;
    pthread_mutex_lock(&gState.lock);
    if (!gState.ready) ensureInitializeLocked();
    pthread_mutex_unlock(&gState.lock);
}

QCamera3MmsGl::Status QCamera3MmsGl::lastStatus() {
    if (!enabled()) return Status{0, 0};
    pthread_mutex_lock(&gState.lock);
    const Status snapshot = gState.status;
    pthread_mutex_unlock(&gState.lock);
    return snapshot;
}

int32_t QCamera3MmsGl::renderIfEnabled(const Image &source, const Image &destination,
        const Vertex *vertices, uint32_t columns, uint32_t rows,
        const std::atomic<bool> *cancelled) {
    if (!enabled()) return kNotHandled;
    pthread_mutex_lock(&gState.lock);
    int32_t status = 0;
    if (gState.disabled || (gState.initialized && !gState.ready)) {
        status = failLocked(gState.initError ? gState.initError : -ENODEV);
    } else if (ensureInitializeLocked() != 0) {
        status = failLocked(gState.initError);
    } else if (!validImage(source) || !validImage(destination) || !vertices ||
            columns < 2 || rows < 2 || columns > kMaxGrid || rows > kMaxGrid) {
        status = failLocked(-EINVAL);
    } else if (isCancelled(cancelled)) {
        status = -ECANCELED;
    } else if (size_t(destination.width) * destination.height * 5 / 2 > kMemoryLimit) {
        status = failLocked(-E2BIG);
    } else {
        /* Same vertex range check as the CPU reference renderer. */
        for (size_t i = 0; i < size_t(columns) * rows; ++i) {
            const Vertex &vertex = vertices[i];
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
                    !std::isfinite(vertex.z) || std::fabs(vertex.x) > 16.0f ||
                    std::fabs(vertex.y) > 16.0f || std::fabs(vertex.z) > 1.0f) {
                status = -ERANGE;
                break;
            }
        }
        if (status == -ERANGE) {
            status = failLocked(status);
        } else if ((status = ensureMeshLocked(columns, rows)) != 0) {
            status = failLocked(status);
        } else if (!eglMakeCurrent(gState.display, gState.surface, gState.surface,
                        gState.context)) {
            if (gState.failuresLogged < kMaxFailureLogs)
                ALOGE("eglMakeCurrent failed (0x%04x)", unsigned(eglGetError()));
            gState.disabled = true;
            gState.initError = -EIO;
            status = failLocked(-EIO);
        } else {
            status = ensureBuffersLocked(source.width, source.height,
                    destination.width, destination.height);
            if (status == 0) status = uploadLocked(source);
            if (status == 0) status = drawLocked(vertices, destination);
            /* Cancelled before readback leaves destination untouched. */
            if (status == 0 && isCancelled(cancelled)) status = -ECANCELED;
            if (status == 0) status = readbackLocked(destination);
            eglMakeCurrent(gState.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (status == 0) {
                gState.consecutiveFailures = 0;
                setStatusLocked(1, 0);
            } else if (status == -ECANCELED) {
                /* Cancellation is not a backend failure. */
            } else {
                /* Latch off after a run of backend failures; input errors do not count. */
                ++gState.consecutiveFailures;
                if (gState.consecutiveFailures >= kMaxFailureLogs) {
                    gState.disabled = true;
                    ALOGE("GPU execution unit disabled after %d consecutive failures (%d)",
                            gState.consecutiveFailures, status);
                }
                gState.initError = status;
                status = failLocked(status);
            }
        }
    }
    pthread_mutex_unlock(&gState.lock);
    return status;
}

} // namespace qcamera
