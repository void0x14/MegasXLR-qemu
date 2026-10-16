/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GPU scanout ownership is handed to one hardware encoder by DMA-BUF FD.
 * The display listener never reads, allocates or copies raw pixel storage. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/dmabuf.h"
#include "ui/egl-context.h"
#include "ui/egl-helpers.h"
#include "megas-video.h"
#include <poll.h>
#include <xf86drm.h>

#define INVALID_FENCE (-1)
typedef struct MegasDisplay {
    DisplayChangeListener dcl;
    DisplayGLCtx glctx;
    QemuThread worker;
    QemuMutex lock;
    QemuCond ready;
    QEMUBH *completed;
    MegasDmaFrame scanout, job;
    QemuDmaBuf *borrowed_scanout;
    int scanout_fence, job_fence;
    bool stop, connected, busy, pending, dirty, source_valid;
    unsigned fps, bitrate;
    char *rendernode;
    int listener, client;
    int64_t captured_ms, published_ms;
    uint8_t transport[188 * 32];
    size_t transport_used;
} MegasDisplay;

static MegasDisplay *display;
static const DisplayChangeListenerOps megas_ops;

static void frame_clear(MegasDmaFrame *frame)
{
    unsigned i;
    for (i = 0; i < frame->planes; i++) {
        if (frame->fd[i] >= 0) {
            close(frame->fd[i]);
        }
    }
    frame->planes = 0;
}

static bool frame_reference(MegasDmaFrame *destination,
                             const MegasDmaFrame *source)
{
    unsigned i;
    *destination = *source; /* descriptor metadata only, never pixel data */
    for (i = 0; i < MEGAS_DMA_PLANES; i++) {
        destination->fd[i] = -1;
    }
    for (i = 0; i < source->planes; i++) {
        destination->fd[i] = fcntl(source->fd[i], F_DUPFD_CLOEXEC, 0);
        if (destination->fd[i] < 0) {
            frame_clear(destination);
            return false;
        }
    }
    return true;
}

static bool same_shape(const MegasDmaFrame *a, const MegasDmaFrame *b)
{
    return a->width == b->width && a->height == b->height &&
           a->backing_width == b->backing_width &&
           a->backing_height == b->backing_height &&
           a->x == b->x && a->y == b->y && a->fourcc == b->fourcc &&
           a->modifier == b->modifier && a->y0_top == b->y0_top;
}

static bool megas_flush(MegasDisplay *s)
{
    size_t sent = 0;
    while (sent < s->transport_used) {
        ssize_t n = send(s->client, s->transport + sent,
                         s->transport_used - sent, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        sent += n;
    }
    s->transport_used = 0;
    return true;
}

static bool megas_write(void *opaque, const uint8_t *data, size_t size)
{
    MegasDisplay *s = opaque;
    assert(size == 188);
    if (s->transport_used + size > sizeof(s->transport) && !megas_flush(s)) {
        return false;
    }
    /* Encoded TS packets only. This is not a raw-image copy. */
    memcpy(s->transport + s->transport_used, data, size);
    s->transport_used += size;
    return true;
}

static void megas_disconnect(MegasDisplay *s)
{
    if (s->client >= 0) {
        close(s->client);
    }
    s->client = -1;
    s->transport_used = 0;
    qemu_mutex_lock(&s->lock);
    s->connected = false;
    qemu_mutex_unlock(&s->lock);
}

/* Runs under the main loop, after the worker has released imported GPU frames. */
static void megas_completed(void *opaque)
{
    MegasDisplay *s = opaque;
    qemu_mutex_lock(&s->lock);
    frame_clear(&s->job);
    if (s->job_fence >= 0) {
        close(s->job_fence);
        s->job_fence = INVALID_FENCE;
    }
    s->busy = false;
    qemu_mutex_unlock(&s->lock);
    qemu_console_hw_gl_block(s->dcl.con, false);
}

static bool producer_ready(int fd)
{
    struct pollfd fence = { .fd = fd, .events = POLLIN };
    int result;
    if (fd < 0) {
        return true; /* kernel DMA-BUF implicit synchronization */
    }
    do {
        result = poll(&fence, 1, 500);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (fence.revents & POLLIN);
}

static void *megas_worker(void *opaque)
{
    MegasDisplay *s = opaque;
    MegasVideo *encoder = NULL;
    MegasDmaFrame shape = {0};
    int64_t last_keyframe = -90000, last_write_ms = 0;
    bool force_keyframe = true;
    char error[512];

    for (;;) {
        int64_t timestamp, now;
        if (s->client < 0) {
            s->client = qemu_accept(s->listener, NULL, NULL);
            if (s->client >= 0) {
                if (!qemu_set_blocking(s->client, false, NULL)) {
                    close(s->client);
                    s->client = -1;
                } else {
                    socket_set_nodelay(s->client);
                    force_keyframe = true;
                    qemu_mutex_lock(&s->lock);
                    s->connected = true;
                    qemu_mutex_unlock(&s->lock);
                }
            }
        }
        qemu_mutex_lock(&s->lock);
        if (!s->stop && !s->pending) {
            qemu_cond_timedwait(&s->ready, &s->lock, 20);
        }
        if (s->stop) {
            qemu_mutex_unlock(&s->lock);
            break;
        }
        if (!s->pending) {
            qemu_mutex_unlock(&s->lock);
            now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            if (encoder && s->client >= 0 && now - last_write_ms >= 40) {
                if (!megas_video_keepalive(encoder, now * 90, megas_write, s) ||
                    !megas_flush(s)) {
                    megas_disconnect(s);
                }
                last_write_ms = now;
            }
            continue;
        }
        s->pending = false;
        timestamp = s->captured_ms * 90;
        qemu_mutex_unlock(&s->lock);
        error[0] = 0;
        if (!producer_ready(s->job_fence)) {
            snprintf(error, sizeof(error), "producer GPU fence did not complete within 500 ms");
            goto failed;
        }
        if (!encoder || !same_shape(&shape, &s->job)) {
            megas_video_free(encoder);
            encoder = megas_video_new(&s->job, s->fps, s->bitrate,
                                       s->rendernode, error, sizeof(error));
            shape = s->job;
            force_keyframe = true;
            if (!encoder) {
                goto failed;
            }
        }
        force_keyframe |= timestamp - last_keyframe >= 90000;
        if (!megas_video_encode(encoder, &s->job, timestamp, force_keyframe,
                                megas_write, s, error, sizeof(error)) ||
            !megas_flush(s)) {
            goto failed;
        }
        if (force_keyframe) {
            last_keyframe = timestamp;
        }
        force_keyframe = false;
        last_write_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        qemu_bh_schedule(s->completed);
        continue;
failed:
        error_report("megas GPU stream: %s", error[0] ? error : "viewer backpressure");
        megas_video_free(encoder);
        encoder = NULL;
        megas_disconnect(s);
        qemu_bh_schedule(s->completed);
    }
    megas_video_free(encoder);
    megas_disconnect(s);
    return NULL;
}

static void megas_submit(MegasDisplay *s)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (!s->source_valid || now - s->published_ms < DIV_ROUND_UP(1000, s->fps)) {
        return;
    }
    qemu_mutex_lock(&s->lock);
    if (!s->connected || s->busy || (!s->dirty && now - s->published_ms < 1000)) {
        qemu_mutex_unlock(&s->lock);
        return;
    }
    if (!frame_reference(&s->job, &s->scanout)) {
        qemu_mutex_unlock(&s->lock);
        return;
    }
    s->job_fence = s->scanout_fence < 0 ? INVALID_FENCE :
                    fcntl(s->scanout_fence, F_DUPFD_CLOEXEC, 0);
    if (s->scanout_fence >= 0 && s->job_fence < 0) {
        frame_clear(&s->job);
        qemu_mutex_unlock(&s->lock);
        return;
    }
    s->busy = true;
    s->pending = true;
    s->dirty = false;
    s->captured_ms = s->published_ms = now;
    /* A borrowed allocation is not an immutable snapshot. Hold producer reuse
     * until hardware completion; duplicating its FD alone is insufficient. */
    qemu_console_hw_gl_block(s->dcl.con, true);
    qemu_cond_signal(&s->ready);
    qemu_mutex_unlock(&s->lock);
}

static void megas_refresh(DisplayChangeListener *dcl)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    qemu_console_hw_update(dcl->con);
    megas_submit(s);
}

static void megas_disable(DisplayChangeListener *dcl)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    frame_clear(&s->scanout);
    s->source_valid = false;
    s->borrowed_scanout = NULL;
    if (s->scanout_fence >= 0) {
        close(s->scanout_fence);
        s->scanout_fence = INVALID_FENCE;
    }
}

static void megas_texture(DisplayChangeListener *dcl, uint32_t texture,
                           bool y0_top, uint32_t backing_width,
                           uint32_t backing_height, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, void *d3d_texture)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    int planes;
    MegasDmaFrame frame = { .width = width, .height = height,
        .backing_width = backing_width, .backing_height = backing_height,
        .x = x, .y = y, .y0_top = y0_top };
    megas_disable(dcl);
    if (!egl_dmabuf_export_texture(texture, frame.fd, (EGLint *)frame.offset,
                                    (EGLint *)frame.stride, (EGLint *)&frame.fourcc,
                                    &planes, &frame.modifier)) {
        error_report("megas: GPU texture export failed; CPU readback is disabled");
        return;
    }
    frame.planes = planes;
    for (int i = 0; i < planes; i++) {
        fcntl(frame.fd[i], F_SETFD, FD_CLOEXEC);
    }
    s->scanout = frame;
    s->source_valid = true;
    s->dirty = true;
}

static void megas_dmabuf(DisplayChangeListener *dcl, QemuDmaBuf *buffer)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    MegasDmaFrame frame = {
        .width = qemu_dmabuf_get_width(buffer),
        .height = qemu_dmabuf_get_height(buffer),
        .backing_width = qemu_dmabuf_get_backing_width(buffer),
        .backing_height = qemu_dmabuf_get_backing_height(buffer),
        .x = qemu_dmabuf_get_x(buffer), .y = qemu_dmabuf_get_y(buffer),
        .fourcc = qemu_dmabuf_get_fourcc(buffer),
        .modifier = qemu_dmabuf_get_modifier(buffer),
        .y0_top = qemu_dmabuf_get_y0_top(buffer),
        .planes = qemu_dmabuf_get_num_planes(buffer),
    };
    int count, strides, offsets;
    const int *fds = qemu_dmabuf_get_fds(buffer, &count);
    const uint32_t *stride = qemu_dmabuf_get_strides(buffer, &strides);
    const uint32_t *offset = qemu_dmabuf_get_offsets(buffer, &offsets);
    unsigned i;
    megas_disable(dcl);
    if (!frame.planes || frame.planes > MEGAS_DMA_PLANES ||
        count < (int)frame.planes || strides < (int)frame.planes ||
        offsets < (int)frame.planes) {
        error_report("megas: invalid GPU plane metadata");
        return;
    }
    for (i = 0; i < frame.planes; i++) {
        frame.fd[i] = fds[i];
        frame.stride[i] = stride[i];
        frame.offset[i] = offset[i];
    }
    if (frame_reference(&s->scanout, &frame)) {
        s->source_valid = true;
        s->borrowed_scanout = buffer;
        s->dirty = true;
    }
}

static void megas_release(DisplayChangeListener *dcl, QemuDmaBuf *buffer)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    if (s->borrowed_scanout == buffer) {
        megas_disable(dcl);
    }
}

static void megas_gl_update(DisplayChangeListener *dcl, uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    EGLSyncKHR sync;
    if (s->scanout_fence >= 0) {
        close(s->scanout_fence);
        s->scanout_fence = INVALID_FENCE;
    }
    sync = egl_create_sync();
    glFlush();
    if (sync != EGL_NO_SYNC_KHR) {
        s->scanout_fence = egl_create_fence(sync);
    }
    s->dirty = true;
    megas_submit(s);
}

static const DisplayChangeListenerOps megas_ops = {
    .dpy_name = "megas",
    .dpy_refresh = megas_refresh,
    .dpy_gl_scanout_disable = megas_disable,
    .dpy_gl_scanout_texture = megas_texture,
    .dpy_gl_scanout_dmabuf = megas_dmabuf,
    .dpy_gl_release_dmabuf = megas_release,
    .dpy_gl_update = megas_gl_update,
};

static QEMUGLContext megas_create_context(DisplayGLCtx *context,
                                          QEMUGLParams *parameters)
{
    return qemu_egl_create_context(context, parameters, qemu_egl_rn_ctx);
}

static bool megas_compatible(DisplayGLCtx *context, DisplayChangeListener *listener)
{
    return listener->ops == &megas_ops;
}

static const DisplayGLCtxOps megas_context_ops = {
    .dpy_gl_ctx_is_compatible_dcl = megas_compatible,
    .dpy_gl_ctx_create = megas_create_context,
    .dpy_gl_ctx_destroy = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
};

static bool megas_probe_write(void *opaque, const uint8_t *data, size_t size)
{
    size_t *bytes = opaque;
    *bytes += size;
    return true;
}

/* Exercise the same allocation/import/encode path before choosing a device.
 * No CPU image or vendor/model table participates in selection. */
static bool megas_probe_device(const char *node, DisplayGLMode mode,
                              char *reason, size_t capacity)
{
    const char *renderer;
    g_autofree char *lower = NULL;
    Error *error = NULL;
    MegasDmaFrame frame = { .width = 128, .height = 96,
        .backing_width = 128, .backing_height = 96, .y0_top = true };
    MegasVideo *encoder = NULL;
    GLuint texture = 0, framebuffer = 0;
    EGLint planes = 0;
    size_t bytes = 0;
    bool ok = false;

    if (!egl_init(node, mode, &error)) {
        snprintf(reason, capacity, "%s", error_get_pretty(error));
        error_free(error);
        goto out;
    }
    renderer = (const char *)glGetString(GL_RENDERER);
    lower = renderer ? g_ascii_strdown(renderer, -1) : NULL;
    if (!lower || strstr(lower, "llvmpipe") || strstr(lower, "softpipe") ||
        strstr(lower, "lavapipe") || strstr(lower, "swiftshader") ||
        strstr(lower, "software rasterizer")) {
        snprintf(reason, capacity, "software GL renderer rejected");
        goto out;
    }
    if (!epoxy_has_egl_extension(qemu_egl_display,
                                 "EGL_MESA_image_dma_buf_export")) {
        snprintf(reason, capacity, "GPU texture DMA-BUF export unavailable");
        goto out;
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(reason, capacity, "GPU probe framebuffer unavailable");
        goto out;
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish(); /* one startup probe; normal streaming uses native fences */
    if (!egl_dmabuf_export_texture(texture, frame.fd, (EGLint *)frame.offset,
                                   (EGLint *)frame.stride,
                                   (EGLint *)&frame.fourcc, &planes,
                                   &frame.modifier)) {
        snprintf(reason, capacity, "GPU probe DMA-BUF export failed");
        goto out;
    }
    frame.planes = planes;
    encoder = megas_video_new(&frame, 30, 500, node, reason, capacity);
    if (!encoder || !megas_video_encode(encoder, &frame, 90000, true,
                                        megas_probe_write, &bytes,
                                        reason, capacity)) {
        goto out;
    }
    ok = bytes > 0;
    if (!ok) {
        snprintf(reason, capacity, "GPU probe produced no encoded output");
    }
out:
    megas_video_free(encoder);
    frame_clear(&frame);
    if (framebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
    }
    if (texture) {
        glDeleteTextures(1, &texture);
    }
    if (!ok) {
        egl_cleanup();
    }
    return ok;
}

static void megas_early_init(DisplayOptions *options)
{
    DisplayMegas *config = &options->u.megas;
    g_autofree drmDevicePtr *devices = NULL;
    char reason[512] = "no accessible DRM render device";
    int count, capacity, i;
    bool found = false;

    if (!options->has_gl || options->gl == DISPLAY_GL_MODE_OFF) {
        error_report("megas requires gl=on; no CPU-copy fallback");
        exit(1);
    }
    if (config->rendernode && config->rendernode[0]) {
        found = megas_probe_device(config->rendernode, options->gl,
                                   reason, sizeof(reason));
    } else {
        count = drmGetDevices2(0, NULL, 0);
        if (count > 0) {
            capacity = count;
            devices = g_new0(drmDevicePtr, capacity);
            count = MIN(drmGetDevices2(0, devices, capacity), capacity);
        }
        for (i = 0; i < count; i++) {
            const char *node;
            if (!devices[i] ||
                !(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) {
                continue;
            }
            node = devices[i]->nodes[DRM_NODE_RENDER];
            if (megas_probe_device(node, options->gl, reason, sizeof(reason))) {
                g_free(config->rendernode);
                config->rendernode = g_strdup(node);
                found = true;
                break;
            }
        }
        if (count > 0) {
            drmFreeDevices(devices, count);
        }
    }
    if (!found) {
        error_report("megas: no usable GPU render/import/encode path: %s; "
                     "no CPU-copy fallback", reason);
        exit(1);
    }
}

static void megas_cleanup(void)
{
    MegasDisplay *s = display;
    if (!s) {
        return;
    }
    qemu_console_unregister_listener(&s->dcl);
    qemu_mutex_lock(&s->lock);
    s->stop = true;
    qemu_cond_signal(&s->ready);
    qemu_mutex_unlock(&s->lock);
    qemu_thread_join(&s->worker);
    qemu_bh_delete(s->completed);
    if (s->busy) {
        megas_completed(s);
    }
    megas_disable(&s->dcl);
    frame_clear(&s->job);
    qemu_console_set_display_gl_ctx(s->dcl.con, NULL);
    close(s->listener);
    qemu_cond_destroy(&s->ready);
    qemu_mutex_destroy(&s->lock);
    g_free(s->rendernode);
    g_free(s);
    display = NULL;
    egl_cleanup();
}

static void megas_init(DisplayState *ds, DisplayOptions *opts)
{
    DisplayMegas *options = &opts->u.megas;
    MegasDisplay *s;
    char port[8], host[] = "127.0.0.1";
    SocketAddress address = { .type = SOCKET_ADDRESS_TYPE_INET,
        .u.inet.host = host, .u.inet.port = port };
    if (!options->port ||
        (options->has_fps && (!options->fps || options->fps > 60)) ||
        (options->has_bitrate && (!options->bitrate || options->bitrate > 20000))) {
        error_report("megas requires port 1..65535, fps 1..60, bitrate 1..20000 kbit/s");
        exit(1);
    }
    s = g_new0(MegasDisplay, 1);
    s->client = -1;
    s->scanout_fence = s->job_fence = INVALID_FENCE;
    s->fps = options->has_fps ? options->fps : 30;
    s->bitrate = options->has_bitrate ? options->bitrate : 1500;
    s->rendernode = g_strdup(options->rendernode);
    snprintf(port, sizeof(port), "%u", (unsigned)options->port);
    s->listener = socket_listen(&address, 1, &error_fatal);
    qemu_set_blocking(s->listener, false, &error_fatal);
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->ready);
    s->completed = qemu_bh_new(megas_completed, s);
    s->glctx.ops = &megas_context_ops;
    s->dcl.con = qemu_console_lookup_default();
    qemu_console_set_display_gl_ctx(s->dcl.con, &s->glctx);
    qemu_thread_create(&s->worker, "megas-gpu", megas_worker, s, QEMU_THREAD_JOINABLE);
    display = s;
    qemu_console_register_listener(s->dcl.con, &s->dcl, &megas_ops);
    qemu_console_listener_set_refresh(&s->dcl, DIV_ROUND_UP(1000, s->fps));
}

static QemuDisplay megas_display = {
    .type = DISPLAY_TYPE_MEGAS,
    .early_init = megas_early_init,
    .init = megas_init,
    .cleanup = megas_cleanup,
};
static void register_megas(void)
{
    qemu_display_register(&megas_display);
}
type_init(register_megas);
module_dep("ui-opengl");
