/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DMA-BUF -> direct VAAPI import -> GPU VPP -> H.264 hardware encoder.
 * There is deliberately no software encoder, upload or download path. */
#if defined(MEGAS_VIDEO_STANDALONE) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef MEGAS_VIDEO_STANDALONE
#include "qemu/osdep.h"
#endif
#include "megas-video.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>

struct MegasVideo {
    AVBufferRef *device, *frames;
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    AVCodecContext *encoder;
    MegasMpegTs transport;
};

static bool failure(char *error, size_t capacity, const char *operation, int code)
{
    char reason[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, reason, sizeof(reason));
    snprintf(error, capacity, "%s: %s; no CPU-copy fallback", operation, reason);
    return false;
}

static int create_filter(AVFilterContext **context, const char *name,
                         const char *instance, const char *arguments,
                         AVFilterGraph *graph)
{
    const AVFilter *filter = avfilter_get_by_name(name);
    return filter ? avfilter_graph_create_filter(context, filter, instance,
                                                 arguments, NULL, graph) :
                    AVERROR_FILTER_NOT_FOUND;
}

static enum AVPixelFormat source_format(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888: return AV_PIX_FMT_BGR0;
    case DRM_FORMAT_ARGB8888: return AV_PIX_FMT_BGRA;
    case DRM_FORMAT_XBGR8888: return AV_PIX_FMT_RGB0;
    case DRM_FORMAT_ABGR8888: return AV_PIX_FMT_RGBA;
    case DRM_FORMAT_NV12: return AV_PIX_FMT_NV12;
    case DRM_FORMAT_YUV420: return AV_PIX_FMT_YUV420P;
    default: return AV_PIX_FMT_NONE;
    }
}

static void descriptor_free(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *descriptor = (AVDRMFrameDescriptor *)data;
    (void)opaque;
    int i;
    for (i = 0; i < descriptor->nb_objects; i++) {
        close(descriptor->objects[i].fd);
    }
    av_free(data);
}

static AVFrame *import_descriptor(MegasVideo *v, const MegasDmaFrame *input,
                                  int64_t timestamp)
{
    AVFrame *frame = av_frame_alloc();
    AVDRMFrameDescriptor *descriptor = av_mallocz(sizeof(*descriptor));
    struct stat identities[MEGAS_DMA_PLANES];
    unsigned plane;

    if (!frame || !descriptor) {
        av_frame_free(&frame);
        av_free(descriptor);
        return NULL;
    }
    descriptor->nb_layers = 1;
    descriptor->layers[0].format = input->fourcc;
    descriptor->layers[0].nb_planes = input->planes;
    for (plane = 0; plane < input->planes; plane++) {
        struct stat identity;
        int object;
        if (fstat(input->fd[plane], &identity) < 0) {
            goto fail;
        }
        for (object = 0; object < descriptor->nb_objects; object++) {
            if (identity.st_dev == identities[object].st_dev &&
                identity.st_ino == identities[object].st_ino) {
                break;
            }
        }
        if (object == descriptor->nb_objects) {
            int fd = fcntl(input->fd[plane], F_DUPFD_CLOEXEC, 0);
            off_t size;
            if (fd < 0) {
                goto fail;
            }
            size = lseek(fd, 0, SEEK_END);
            if (size <= 0) {
                close(fd);
                goto fail;
            }
            identities[object] = identity;
            descriptor->objects[object].fd = fd;
            descriptor->objects[object].size = size;
            descriptor->objects[object].format_modifier = input->modifier;
            descriptor->nb_objects++;
        }
        descriptor->layers[0].planes[plane].object_index = object;
        descriptor->layers[0].planes[plane].offset = input->offset[plane];
        descriptor->layers[0].planes[plane].pitch = input->stride[plane];
    }
    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = input->backing_width;
    frame->height = input->backing_height;
    frame->pts = timestamp;
    frame->crop_left = input->x;
    frame->crop_top = input->y;
    frame->crop_right = input->backing_width - input->x - input->width;
    frame->crop_bottom = input->backing_height - input->y - input->height;
    frame->hw_frames_ctx = av_buffer_ref(v->frames);
    frame->buf[0] = av_buffer_create((uint8_t *)descriptor, sizeof(*descriptor),
                                    descriptor_free, NULL, 0);
    if (!frame->buf[0]) {
        goto fail;
    }
    frame->data[0] = frame->buf[0]->data;
    if (!frame->hw_frames_ctx) {
        av_frame_free(&frame);
        return NULL;
    }
    return frame;
fail:
    descriptor_free(NULL, (uint8_t *)descriptor);
    av_frame_free(&frame);
    return NULL;
}

static bool valid_frame(const MegasDmaFrame *f)
{
    unsigned i;
    if (!f || !f->width || !f->height || ((f->width | f->height) & 1) ||
        f->backing_width > 8192 || f->backing_height > 8192 ||
        f->x > f->backing_width || f->y > f->backing_height ||
        f->width > f->backing_width - f->x ||
        f->height > f->backing_height - f->y ||
        !f->planes || f->planes > MEGAS_DMA_PLANES ||
        source_format(f->fourcc) == AV_PIX_FMT_NONE) {
        return false;
    }
    for (i = 0; i < f->planes; i++) {
        if (f->fd[i] < 0 || !f->stride[i]) {
            return false;
        }
    }
    return true;
}

MegasVideo *megas_video_new(const MegasDmaFrame *input, unsigned fps,
                           unsigned bitrate, const char *rendernode,
                           char *error, size_t capacity)
{
    MegasVideo *v = calloc(1, sizeof(*v));
    AVHWFramesContext *frames;
    AVBufferSrcParameters *parameters = NULL;
    AVFilterContext *map, *scale, *flip = NULL;
    const AVCodec *codec;
    AVDictionary *options = NULL;
    char arguments[256];
    int result = AVERROR(EINVAL);
    const char *operation = "validate DMA-BUF configuration";

    if (!v) {
        failure(error, capacity, "allocate encoder metadata", AVERROR(ENOMEM));
        return NULL;
    }
    if (!valid_frame(input) || !fps || fps > 60 || !bitrate || bitrate > 20000 ||
        !rendernode || strncmp(rendernode, "/dev/dri/renderD", 16)) {
        goto fail;
    }
    operation = "open DRM render device";
    result = av_hwdevice_ctx_create(&v->device, AV_HWDEVICE_TYPE_DRM,
                                    rendernode, NULL, 0);
    if (result < 0) {
        goto fail;
    }
    v->frames = av_hwframe_ctx_alloc(v->device);
    if (!v->frames) {
        result = AVERROR(ENOMEM);
        goto fail;
    }
    frames = (AVHWFramesContext *)v->frames->data;
    frames->format = AV_PIX_FMT_DRM_PRIME;
    frames->sw_format = source_format(input->fourcc);
    frames->width = input->backing_width;
    frames->height = input->backing_height;
    operation = "initialize DMA-BUF descriptor context";
    if ((result = av_hwframe_ctx_init(v->frames)) < 0) {
        goto fail;
    }
    v->graph = avfilter_graph_alloc();
    if (!v->graph) {
        result = AVERROR(ENOMEM);
        goto fail;
    }
    v->graph->nb_threads = 1;
    avfilter_graph_set_auto_convert(v->graph, AVFILTER_AUTO_CONVERT_NONE);
    operation = "create hardware-only filter graph";
    snprintf(arguments, sizeof(arguments),
             "video_size=%ux%u:pix_fmt=%d:time_base=1/90000:pixel_aspect=1/1",
             input->backing_width, input->backing_height, AV_PIX_FMT_DRM_PRIME);
    result = create_filter(&v->source, "buffer", "source", arguments, v->graph);
    if (result < 0) {
        goto fail;
    }
    parameters = av_buffersrc_parameters_alloc();
    if (!parameters) {
        result = AVERROR(ENOMEM);
        goto fail;
    }
    parameters->format = AV_PIX_FMT_DRM_PRIME;
    parameters->hw_frames_ctx = v->frames;
    result = av_buffersrc_parameters_set(v->source, parameters);
    av_freep(&parameters);
    if (result < 0) {
        goto fail;
    }
    result = create_filter(&map, "hwmap", "direct-import",
                           "derive_device=vaapi:mode=read+direct", v->graph);
    if (result < 0 || (result = avfilter_link(v->source, 0, map, 0)) < 0) {
        goto fail;
    }
    snprintf(arguments, sizeof(arguments), "w=%u:h=%u:format=nv12",
             input->width, input->height);
    result = create_filter(&scale, "scale_vaapi", "gpu-color-conversion",
                           arguments, v->graph);
    if (result < 0 || (result = avfilter_link(map, 0, scale, 0)) < 0) {
        goto fail;
    }
    if (!input->y0_top) {
        result = create_filter(&flip, "transpose_vaapi", "gpu-orientation",
                               "dir=vflip", v->graph);
        if (result < 0 || (result = avfilter_link(scale, 0, flip, 0)) < 0) {
            goto fail;
        }
    }
    result = create_filter(&v->sink, "buffersink", "output", NULL, v->graph);
    if (result < 0 ||
        (result = avfilter_link(flip ? flip : scale, 0, v->sink, 0)) < 0 ||
        (result = avfilter_graph_config(v->graph, NULL)) < 0) {
        goto fail;
    }
    operation = "initialize hardware H.264 encoder";
    codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec || !(v->encoder = avcodec_alloc_context3(codec))) {
        result = codec ? AVERROR(ENOMEM) : AVERROR_ENCODER_NOT_FOUND;
        goto fail;
    }
    v->encoder->width = input->width;
    v->encoder->height = input->height;
    v->encoder->pix_fmt = AV_PIX_FMT_VAAPI;
    v->encoder->time_base = (AVRational){1, 90000};
    v->encoder->framerate = (AVRational){fps, 1};
    v->encoder->max_b_frames = 0;
    v->encoder->gop_size = fps;
    v->encoder->thread_count = 1;
    v->encoder->bit_rate = bitrate * 1000;
    v->encoder->rc_max_rate = bitrate * 1000;
    v->encoder->rc_buffer_size = bitrate * 500;
    v->encoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    v->encoder->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(v->sink));
    if (!v->encoder->hw_frames_ctx) {
        result = AVERROR(ENOMEM);
        goto fail;
    }
    av_dict_set(&options, "async_depth", "1", 0);
    result = avcodec_open2(v->encoder, codec, &options);
    if (result < 0 || av_dict_count(options)) {
        if (result >= 0) {
            result = AVERROR_OPTION_NOT_FOUND;
        }
        goto fail;
    }
    av_dict_free(&options);
    error[0] = 0;
    return v;
fail:
    failure(error, capacity, operation, result);
    av_free(parameters);
    av_dict_free(&options);
    megas_video_free(v);
    return NULL;
}

bool megas_video_encode(MegasVideo *v, const MegasDmaFrame *input,
                        int64_t timestamp, bool keyframe,
                        MegasPacketWrite write, void *opaque,
                        char *error, size_t capacity)
{
    AVFrame *source = NULL, *converted = NULL;
    AVPacket *packet = NULL;
    int result = AVERROR(EINVAL);
    bool ok = false;

    if (!v || !valid_frame(input) || timestamp < 0 || !write) {
        return failure(error, capacity, "validate imported frame", result);
    }
    source = import_descriptor(v, input, timestamp);
    converted = av_frame_alloc();
    packet = av_packet_alloc();
    if (!source || !converted || !packet) {
        result = AVERROR(ENOMEM);
        goto done;
    }
    if ((result = av_buffersrc_add_frame_flags(v->source, source,
                                               AV_BUFFERSRC_FLAG_KEEP_REF)) < 0 ||
        (result = av_buffersink_get_frame(v->sink, converted)) < 0) {
        goto done;
    }
    if (converted->format != AV_PIX_FMT_VAAPI) {
        result = AVERROR(EINVAL);
        goto done;
    }
    converted->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if ((result = avcodec_send_frame(v->encoder, converted)) < 0 ||
        (result = avcodec_receive_packet(v->encoder, packet)) < 0) {
        goto done;
    }
    if (packet->pts != timestamp || packet->dts != timestamp) {
        result = AVERROR(EINVAL); /* no hidden reordering or delayed frame queue */
        goto done;
    }
    ok = megas_mpegts_frame(&v->transport, packet->data, packet->size,
                            timestamp, packet->flags & AV_PKT_FLAG_KEY,
                            write, opaque);
    result = ok ? 0 : AVERROR(EPIPE);
done:
    if (!ok) {
        failure(error, capacity, "hardware frame submission/completion", result);
    }
    av_packet_free(&packet);
    av_frame_free(&converted);
    av_frame_free(&source);
    return ok;
}

bool megas_video_keepalive(MegasVideo *v, int64_t timestamp,
                           MegasPacketWrite write, void *opaque)
{
    return v && megas_mpegts_clock(&v->transport, timestamp, write, opaque);
}

void megas_video_free(MegasVideo *v)
{
    if (v) {
        avcodec_free_context(&v->encoder);
        avfilter_graph_free(&v->graph);
        av_buffer_unref(&v->frames);
        av_buffer_unref(&v->device);
        free(v);
    }
}
