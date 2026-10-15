/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_MEGAS_VIDEO_H
#define QEMU_MEGAS_VIDEO_H
#include "megas-mpegts.h"
#define MEGAS_DMA_PLANES 4
/* Metadata and borrowed DMA-BUF FDs only. No raw pixel pointers or CPU buffers. */
typedef struct MegasDmaFrame {
    unsigned width, height, backing_width, backing_height, x, y, planes;
    uint32_t fourcc;
    uint64_t modifier;
    bool y0_top;
    int fd[MEGAS_DMA_PLANES];
    uint32_t offset[MEGAS_DMA_PLANES], stride[MEGAS_DMA_PLANES];
} MegasDmaFrame;
typedef struct MegasVideo MegasVideo;
MegasVideo *megas_video_new(const MegasDmaFrame *, unsigned fps,
                           unsigned bitrate_kbps, const char *rendernode,
                           char *error, size_t capacity);
bool megas_video_encode(MegasVideo *, const MegasDmaFrame *, int64_t timestamp,
                        bool keyframe, MegasPacketWrite, void *,
                        char *error, size_t capacity);
bool megas_video_keepalive(MegasVideo *, int64_t, MegasPacketWrite, void *);
void megas_video_free(MegasVideo *);
#endif
