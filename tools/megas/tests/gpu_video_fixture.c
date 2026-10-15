/* SPDX-License-Identifier: GPL-2.0-or-later */
/* All test pixels are drawn by GL on the GPU. CPU code handles descriptors. */
#define _GNU_SOURCE
#include "megas-video.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>

static bool output(void *opaque, const uint8_t *data, size_t size)
{
    return fwrite(data, 1, size, opaque) == size;
}

int main(int argc, char **argv)
{
    MegasDmaFrame frame = { .width = 128, .height = 96,
        .backing_width = 128, .backing_height = 96, .planes = 1,
        .fourcc = DRM_FORMAT_XRGB8888, .stride = {512} };
    char error[512];
    MegasVideo *encoder;
    int drm, i, planes;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLImageKHR image;
    EGLint count, fourcc;
    EGLuint64KHR modifier;
    GLuint texture, framebuffer;
    const EGLint attributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_NONE };
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    if (argc == 2 && !strcmp(argv[1], "--negative")) {
        frame.fd[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        encoder = megas_video_new(&frame, 30, 500, "/dev/dri/renderD65535",
                                   error, sizeof(error));
        assert(!encoder && strstr(error, "no CPU-copy fallback"));
        close(frame.fd[0]);
        puts("negative check only: absent GPU rejected; hardware video NOT qualified");
        return 0;
    }
    if (argc != 2) {
        fputs("usage: gpu-video-fixture /dev/dri/renderD128 > gpu-colors.ts\n", stderr);
        return 2;
    }
    drm = open(argv[1], O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        perror("open real GPU render node");
        return 1;
    }
    gbm = gbm_create_device(drm);
    assert(gbm);
    display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    assert(display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL));
    assert(eglBindAPI(EGL_OPENGL_ES_API));
    assert(eglChooseConfig(display, attributes, &config, 1, &count) && count);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    assert(context != EGL_NO_CONTEXT);
    assert(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
    assert(epoxy_has_egl_extension(display, "EGL_MESA_image_dma_buf_export"));
    fprintf(stderr, "actual GL renderer: %s\n", glGetString(GL_RENDERER));
    assert(!strstr((const char *)glGetString(GL_RENDERER), "llvmpipe"));
    assert(!strstr((const char *)glGetString(GL_RENDERER), "softpipe"));
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 96, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    image = eglCreateImageKHR(display, context, EGL_GL_TEXTURE_2D_KHR,
                               (EGLClientBuffer)(uintptr_t)texture, NULL);
    assert(image != EGL_NO_IMAGE_KHR);
    assert(eglExportDMABUFImageQueryMESA(display, image, &fourcc, &planes, &modifier));
    assert(planes > 0 && planes <= MEGAS_DMA_PLANES);
    assert(eglExportDMABUFImageMESA(display, image, frame.fd,
                                    (EGLint *)frame.stride, (EGLint *)frame.offset));
    frame.fourcc = fourcc;
    frame.planes = planes;
    frame.modifier = modifier;
    encoder = megas_video_new(&frame, 30, 500, argv[1], error, sizeof(error));
    if (!encoder) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    for (i = 0; i < 60; i++) {
        glClearColor(i < 20, i >= 20 && i < 40, i >= 40, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish(); /* fixture-only producer synchronization */
        if (!megas_video_encode(encoder, &frame, 90000 + i * 3000, i == 0,
                                 output, stdout, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
    }
    megas_video_free(encoder);
    for (i = 0; i < planes; i++) {
        close(frame.fd[i]);
    }
    eglDestroyImageKHR(display, image);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    gbm_device_destroy(gbm);
    close(drm);
    return fflush(stdout) != 0;
}
