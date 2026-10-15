/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native encoded display output. No screendump, subprocess or frame queue. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "megas-video.h"

typedef struct MegasDisplay {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    QemuThread worker;
    QemuMutex lock;
    QemuCond ready;
    bool stop, connected, pending, dirty;
    uint8_t *pixels;
    unsigned width, height, fps, bitrate;
    int listener, client;
    int64_t captured_ms, published_ms;
    uint8_t transport[188 * 32];
    size_t transport_used;
} MegasDisplay;

static MegasDisplay *display;

/* Worker owns the socket and encoder; it never waits for a viewer to read. */
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
    memcpy(s->transport + s->transport_used, data, size);
    s->transport_used += size;
    return true;
}

static void megas_disconnect(MegasDisplay *s)
{
    close(s->client);
    s->client = -1;
    s->transport_used = 0;
    qemu_mutex_lock(&s->lock);
    s->connected = false;
    s->pending = false;
    qemu_mutex_unlock(&s->lock);
}

static void *megas_worker(void *opaque)
{
    MegasDisplay *s = opaque;
    MegasVideo *encoder = NULL;
    uint8_t *pixels = NULL;
    unsigned width = 0, height = 0, encoded_width = 0, encoded_height = 0;
    int64_t timestamp, last_keyframe = -90000;
    bool force_keyframe = true;

    for (;;) {
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
        if (!s->stop && (!s->pending || s->client < 0)) {
            qemu_cond_timedwait(&s->ready, &s->lock, 50);
        }
        if (s->stop) {
            qemu_mutex_unlock(&s->lock);
            break;
        }
        if (!s->pending || s->client < 0) {
            qemu_mutex_unlock(&s->lock);
            continue;
        }
        /* At most one latest update waits while the worker encodes. */
        uint8_t *old = pixels;
        if (width != s->width || height != s->height) {
            g_free(old);
            old = NULL;
        }
        pixels = s->pixels;
        s->pixels = old;
        width = s->width;
        height = s->height;
        timestamp = s->captured_ms * 90;
        s->pending = false;
        qemu_mutex_unlock(&s->lock);

        if (!encoder || width != encoded_width || height != encoded_height) {
            megas_video_free(encoder);
            encoder = megas_video_new(width, height, s->fps, s->bitrate);
            encoded_width = width;
            encoded_height = height;
            force_keyframe = true;
            if (!encoder) {
                error_report("megas: cannot initialize H.264 encoder for %ux%u",
                             width, height);
            }
        }
        force_keyframe |= timestamp - last_keyframe >= 90000;
        if (!encoder ||
            !megas_video_encode(encoder, pixels, width * 4, timestamp,
                                force_keyframe, megas_write, s) ||
            !megas_flush(s)) {
            megas_disconnect(s);
            continue;
        }
        if (force_keyframe) {
            last_keyframe = timestamp;
        }
        force_keyframe = false;
    }
    if (s->client >= 0) {
        megas_disconnect(s);
    }
    megas_video_free(encoder);
    g_free(pixels);
    return NULL;
}

static void megas_refresh(DisplayChangeListener *dcl)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    unsigned width, height;
    pixman_image_t *image;
    pixman_format_code_t format;

    qemu_console_hw_update(dcl->con);
    if (!s->surface || qemu_mutex_trylock(&s->lock)) {
        return;
    }
    if (!s->connected || (!s->dirty && now - s->published_ms < 100)) {
        qemu_mutex_unlock(&s->lock);
        return;
    }
    width = (surface_width(s->surface) + 1) & ~1;
    height = (surface_height(s->surface) + 1) & ~1;
    if (!width || !height || width > 4096 || height > 4096) {
        qemu_mutex_unlock(&s->lock);
        return;
    }
    if (width != s->width || height != s->height) {
        g_clear_pointer(&s->pixels, g_free);
    }
    if (!s->pixels) {
        s->pixels = g_try_malloc0((size_t)width * height * 4);
        if (!s->pixels) {
            qemu_mutex_unlock(&s->lock);
            return;
        }
    }
    /* Normalize any software display format to BGRA bytes for libyuv. */
    format = G_BYTE_ORDER == G_LITTLE_ENDIAN ? PIXMAN_a8r8g8b8 :
                                             PIXMAN_b8g8r8a8;
    image = pixman_image_create_bits(format, width, height,
                                     (uint32_t *)s->pixels, width * 4);
    if (image) {
        pixman_image_composite32(PIXMAN_OP_SRC, s->surface->image, NULL, image,
                                 0, 0, 0, 0, 0, 0,
                                 surface_width(s->surface),
                                 surface_height(s->surface));
        pixman_image_unref(image);
        s->width = width;
        s->height = height;
        s->captured_ms = now;
        s->published_ms = now;
        s->dirty = false;
        s->pending = true;
        qemu_cond_signal(&s->ready);
    }
    qemu_mutex_unlock(&s->lock);
}

static void megas_gfx_update(DisplayChangeListener *dcl,
                             int x, int y, int width, int height)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    s->dirty = true;
}

static void megas_gfx_switch(DisplayChangeListener *dcl,
                             DisplaySurface *surface)
{
    MegasDisplay *s = container_of(dcl, MegasDisplay, dcl);
    s->surface = surface;
    s->dirty = true;
}

static bool megas_check_format(DisplayChangeListener *dcl,
                                pixman_format_code_t format)
{
    return pixman_format_supported_source(format);
}

static const DisplayChangeListenerOps megas_ops = {
    .dpy_name = "megas",
    .dpy_refresh = megas_refresh,
    .dpy_gfx_update = megas_gfx_update,
    .dpy_gfx_switch = megas_gfx_switch,
    .dpy_gfx_check_format = megas_check_format,
};

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
    close(s->listener);
    g_free(s->pixels);
    qemu_cond_destroy(&s->ready);
    qemu_mutex_destroy(&s->lock);
    g_free(s);
    display = NULL;
}

static void megas_init(DisplayState *ds, DisplayOptions *opts)
{
    DisplayMegas *options = &opts->u.megas;
    MegasDisplay *s;
    char port[8];
    char host[] = "127.0.0.1";
    SocketAddress address = {
        .type = SOCKET_ADDRESS_TYPE_INET,
        .u.inet.host = host,
        .u.inet.port = port,
    };

    if (!options->port ||
        (options->has_fps && (!options->fps || options->fps > 60)) ||
        (options->has_bitrate && (!options->bitrate || options->bitrate > 20000)) ||
        (opts->has_gl && opts->gl != DISPLAY_GL_MODE_OFF)) {
        error_report("megas display requires port 1..65535, fps 1..60, "
                     "bitrate 1..20000 kbit/s and a software scanout");
        exit(1);
    }
    s = g_new0(MegasDisplay, 1);
    s->client = -1;
    s->fps = options->has_fps ? options->fps : 30;
    s->bitrate = options->has_bitrate ? options->bitrate : 1500;
    snprintf(port, sizeof(port), "%u", (unsigned)options->port);
    s->listener = socket_listen(&address, 1, &error_fatal);
    qemu_set_blocking(s->listener, false, &error_fatal);
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->ready);
    qemu_thread_create(&s->worker, "megas-video", megas_worker, s,
                       QEMU_THREAD_JOINABLE);
    display = s;
    qemu_console_register_listener(qemu_console_lookup_default(), &s->dcl,
                                   &megas_ops);
    qemu_console_listener_set_refresh(&s->dcl, DIV_ROUND_UP(1000, s->fps));
}

static QemuDisplay megas_display = {
    .type = DISPLAY_TYPE_MEGAS,
    .init = megas_init,
    .cleanup = megas_cleanup,
};

static void register_megas(void)
{
    qemu_display_register(&megas_display);
}

type_init(register_megas);
