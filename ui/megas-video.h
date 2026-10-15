/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_MEGAS_VIDEO_H
#define QEMU_MEGAS_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MegasVideo MegasVideo;
/* Return false to abandon this connection; no encoded data is queued. */
typedef bool (*MegasVideoWrite)(void *opaque, const uint8_t *data, size_t size);

MegasVideo *megas_video_new(unsigned width, unsigned height, unsigned fps,
                          unsigned bitrate_kbps);
/* BGRA byte order; timestamp is monotonic, in 90 kHz units. */
bool megas_video_encode(MegasVideo *video, const uint8_t *bgra, int stride,
                        int64_t timestamp, bool force_keyframe,
                        MegasVideoWrite write, void *opaque);
void megas_video_free(MegasVideo *video);

#endif
