/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEGAS_VIDEO_STANDALONE
#include "qemu/osdep.h"
#endif
#include "megas-mpegts.h"
#include <string.h>

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
                    size_t size, MegasPacketWrite write, void *opaque)
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

bool megas_mpegts_clock(MegasMpegTs *v, int64_t timestamp,
                           MegasPacketWrite write, void *opaque)
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

bool megas_mpegts_frame(MegasMpegTs *v, const uint8_t *data, size_t size,
                        uint64_t pts, bool keyframe,
                        MegasPacketWrite write, void *opaque)
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

