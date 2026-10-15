/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_MEGAS_MPEGTS_H
#define QEMU_MEGAS_MPEGTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*MegasPacketWrite)(void *, const uint8_t *, size_t);
typedef struct MegasMpegTs {
    uint8_t pat_cc, pmt_cc, video_cc;
} MegasMpegTs;
bool megas_mpegts_frame(MegasMpegTs *, const uint8_t *, size_t, uint64_t,
                        bool, MegasPacketWrite, void *);
bool megas_mpegts_clock(MegasMpegTs *, int64_t, MegasPacketWrite, void *);
#endif
