/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Single-threaded H.264 encoder and one-program MPEG-TS packetizer. */
#ifndef MEGAS_VIDEO_STANDALONE
#include "qemu/osdep.h"
#endif
#include "megas-video.h"
#include <stdlib.h>
#include <string.h>
#include <x264.h>
#include <libyuv/convert_from_argb.h>

struct MegasVideo {
    x264_t *encoder;
    x264_picture_t picture;
    unsigned width, height;
    int64_t frames;
    uint8_t pat_cc, pmt_cc, video_cc;
};

static uint32_t section_crc(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned bit;

    for (i = 0; i < size; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ (crc & 0x80000000 ? 0x04c11db7 : 0);
        }
    }
    return crc;
}

static bool section(unsigned pid, uint8_t *counter, const uint8_t *data,
                    size_t size, MegasVideoWrite write, void *opaque)
{
    uint8_t packet[188];
    uint32_t crc = section_crc(data, size);

    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47;
    packet[1] = 0x40 | (pid >> 8);
    packet[2] = pid;
    packet[3] = 0x10 | ((*counter)++ & 15);
    packet[4] = 0; /* pointer_field */
    memcpy(packet + 5, data, size);
    packet[5 + size] = crc >> 24;
    packet[6 + size] = crc >> 16;
    packet[7 + size] = crc >> 8;
    packet[8 + size] = crc;
    return write(opaque, packet, sizeof(packet));
}

static void put_pts(uint8_t *p, uint64_t pts)
{
    p[0] = 0x21 | ((pts >> 29) & 14);
    p[1] = pts >> 22;
    p[2] = 1 | ((pts >> 14) & 0xfe);
    p[3] = pts >> 7;
    p[4] = 1 | ((pts << 1) & 0xfe);
}

static void put_pcr(uint8_t *p, uint64_t pts)
{
    p[0] = pts >> 25;
    p[1] = pts >> 17;
    p[2] = pts >> 9;
    p[3] = pts >> 1;
    p[4] = ((pts & 1) << 7) | 0x7e;
    p[5] = 0;
}

bool megas_video_keepalive(MegasVideo *v, int64_t timestamp,
                           MegasVideoWrite write, void *opaque)
{
    uint8_t packet[188];

    if (!v || timestamp < 0 || !write) {
        return false;
    }
    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47;
    packet[1] = 0x01;
    packet[2] = 0;
    /* Adaptation-only packets do not increment the continuity counter. */
    packet[3] = 0x20 | ((v->video_cc - 1) & 15);
    packet[4] = 183;
    packet[5] = 0x10;
    put_pcr(packet + 6, timestamp);
    return write(opaque, packet, sizeof(packet));
}

static bool access_unit(MegasVideo *v, const uint8_t *data, size_t size,
                        uint64_t pts, bool keyframe,
                        MegasVideoWrite write, void *opaque)
{
    /* Program 1, PMT PID 0x1000, video/PCR PID 0x0100, stream type AVC. */
    static const uint8_t pat[] = {
        0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0x00, 0x01, 0xf0, 0x00
    };
    static const uint8_t pmt[] = {
        0x02, 0xb0, 0x12, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0xe1, 0x00, 0xf0, 0x00, 0x1b, 0xe1, 0x00, 0xf0, 0x00
    };
    uint8_t pes[14] = {0, 0, 1, 0xe0, 0, 0, 0x80, 0x80, 5};
    size_t offset = 0, total = sizeof(pes) + size;

    if (!section(0, &v->pat_cc, pat, sizeof(pat), write, opaque) ||
        !section(0x1000, &v->pmt_cc, pmt, sizeof(pmt), write, opaque)) {
        return false;
    }
    put_pts(pes + 9, pts);
    while (offset < total) {
        uint8_t packet[188];
        bool first = offset == 0;
        size_t count = total - offset;
        size_t adaptation, start, i;

        /* First packet carries PCR and random_access_indicator. */
        if (count > (first ? 176 : 184)) {
            count = first ? 176 : 184;
        }
        adaptation = 184 - count;
        memset(packet, 0xff, sizeof(packet));
        packet[0] = 0x47;
        packet[1] = 0x01 | (first ? 0x40 : 0);
        packet[2] = 0;
        packet[3] = (adaptation ? 0x30 : 0x10) | (v->video_cc++ & 15);
        if (adaptation) {
            packet[4] = adaptation - 1;
            if (adaptation > 1) {
                packet[5] = first ? 0x10 | (keyframe ? 0x40 : 0) : 0;
            }
            if (first) {
                put_pcr(packet + 6, pts);
            }
        }
        start = 4 + adaptation;
        for (i = 0; i < count; i++, offset++) {
            packet[start + i] = offset < sizeof(pes) ? pes[offset] :
                                data[offset - sizeof(pes)];
        }
        if (!write(opaque, packet, sizeof(packet))) {
            return false;
        }
    }
    return true;
}

MegasVideo *megas_video_new(unsigned width, unsigned height, unsigned fps,
                          unsigned bitrate_kbps)
{
    MegasVideo *v;
    x264_param_t p;

    if (!width || !height || ((width | height) & 1) || width > 4096 ||
        height > 4096 || !fps || fps > 60 || !bitrate_kbps ||
        bitrate_kbps > 20000) {
        return NULL;
    }
    v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    x264_param_default_preset(&p, "ultrafast", "zerolatency");
    p.i_width = width;
    p.i_height = height;
    p.i_csp = X264_CSP_I420;
    p.i_threads = 1;
    p.i_lookahead_threads = 1;
    p.b_sliced_threads = 0;
    p.i_fps_num = fps;
    p.i_fps_den = 1;
    /* x264 VFR adds a frame of lookahead even with the zerolatency tune.
     * Encode immediately with nominal CFR; actual capture PTS belong to TS. */
    p.b_vfr_input = 0;
    p.i_timebase_num = 1;
    p.i_timebase_den = fps;
    p.i_keyint_max = fps; /* reconnect waits at most one second */
    p.i_keyint_min = 1;
    p.b_repeat_headers = 1;
    p.b_annexb = 1;
    p.b_aud = 1;
    p.rc.i_rc_method = X264_RC_ABR;
    p.rc.i_bitrate = bitrate_kbps;
    p.rc.i_vbv_max_bitrate = bitrate_kbps;
    p.rc.i_vbv_buffer_size = bitrate_kbps / 2 + 1;
    p.i_log_level = X264_LOG_WARNING;
    if (x264_param_apply_profile(&p, "baseline") < 0 ||
        x264_picture_alloc(&v->picture, X264_CSP_I420, width, height) < 0) {
        free(v);
        return NULL;
    }
    v->encoder = x264_encoder_open(&p);
    if (!v->encoder || x264_encoder_maximum_delayed_frames(v->encoder)) {
        if (v->encoder) {
            x264_encoder_close(v->encoder);
        }
        x264_picture_clean(&v->picture);
        free(v);
        return NULL;
    }
    v->width = width;
    v->height = height;
    return v;
}

bool megas_video_encode(MegasVideo *v, const uint8_t *bgra, int stride,
                        int64_t timestamp, bool force_keyframe,
                        MegasVideoWrite write, void *opaque)
{
    x264_nal_t *nals;
    x264_picture_t output;
    int count, size;

    if (!v || !bgra || !write || stride < (int)v->width * 4 || timestamp < 0 ||
        ARGBToI420(bgra, stride, v->picture.img.plane[0],
                   v->picture.img.i_stride[0], v->picture.img.plane[1],
                   v->picture.img.i_stride[1], v->picture.img.plane[2],
                   v->picture.img.i_stride[2], v->width, v->height)) {
        return false;
    }
    v->picture.i_pts = v->frames++;
    v->picture.i_type = force_keyframe ? X264_TYPE_IDR : X264_TYPE_AUTO;
    size = x264_encoder_encode(v->encoder, &nals, &count, &v->picture, &output);
    if (size <= 0 || count <= 0) {
        return false;
    }
    /* x264 guarantees all NAL payloads in one contiguous allocation. */
    return access_unit(v, nals[0].p_payload, size, timestamp,
                       output.b_keyframe, write, opaque);
}

void megas_video_free(MegasVideo *v)
{
    if (v) {
        x264_encoder_close(v->encoder);
        x264_picture_clean(&v->picture);
        free(v);
    }
}
