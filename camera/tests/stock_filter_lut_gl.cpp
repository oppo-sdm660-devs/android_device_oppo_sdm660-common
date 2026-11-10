/* SPDX-License-Identifier: BSD-3-Clause */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static GLuint shader(GLenum type, const char *source) {
    GLuint object = glCreateShader(type);
    glShaderSource(object, 1, &source, nullptr);
    glCompileShader(object);
    GLint ok = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(object, sizeof(log), nullptr, log); fprintf(stderr, "%s\n", log); exit(1); }
    return object;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    FILE *file = fopen(argv[1], "rb");
    std::vector<unsigned char> cube(16384);
    if (!file || fread(cube.data(), 1, cube.size(), file) != cube.size() || fgetc(file) != EOF) return 3;
    fclose(file);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) return 4;
    EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint count;
    if (!eglChooseConfig(display, configAttrs, &config, 1, &count) || !count) return 5;
    EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    EGLint surfaceAttrs[] = {EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
    if (!eglMakeCurrent(display, surface, surface, context)) return 6;
    const char *vertex = "attribute vec2 pos; void main(){gl_Position=vec4(pos,0.,1.);}";
    std::ifstream shaderFile(argv[3]);
    std::string fragmentSource((std::istreambuf_iterator<char>(shaderFile)),
            std::istreambuf_iterator<char>());
    const char *fragment = fragmentSource.c_str();
    GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, vertex));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, fragment));
    glBindAttribLocation(program, 0, "pos");
    glLinkProgram(program);
    GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) return 7;
    glUseProgram(program);
    glUniform1f(glGetUniformLocation(program, "lut_size"), 16.f);
    GLuint texture; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, cube.data());
    GLfloat pos[] = {-1,-1, 1,-1, -1,1, 1,1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, 256, 256);
    glDisable(GL_DITHER);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    std::vector<unsigned char> output(256*256*4);
    glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, output.data());
    if (glGetError() != GL_NO_ERROR) return 8;
    file = fopen(argv[2], "wb");
    if (!file || fwrite(output.data(), 1, output.size(), file) != output.size()) return 9;
    fclose(file);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display);
    return 0;
}
