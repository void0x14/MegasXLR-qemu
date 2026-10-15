/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "megas-video.h"
#include <stdio.h>
#include <string.h>

static bool output(void *opaque, const uint8_t *data, size_t size)
{
    return fwrite(data, 1, size, opaque) == size;
}

int main(void)
{
    MegasVideo *v = megas_video_new(128, 96, 30, 500);
    uint8_t pixels[128 * 96 * 4];
    unsigned i, pixel;

    if (!v || megas_video_new(127, 96, 30, 500)) {
        return 1;
    }
    for (i = 0; i < 60; i++) {
        memset(pixels, 0, sizeof(pixels));
        for (pixel = 0; pixel < 128 * 96; pixel++) {
            /* Red -> green -> blue in BGRA byte order. */
            pixels[pixel * 4 + 2 - i / 20] = 255;
            pixels[pixel * 4 + 3] = 255;
        }
        /* Exercise the MPEG-TS 33-bit timestamp wrap. */
        if (!megas_video_encode(v, pixels, 128 * 4,
                                 (INT64_C(1) << 33) - 9000 + i * 3000,
                                 i == 0, output, stdout)) {
            return 2;
        }
        if (!megas_video_keepalive(v, (INT64_C(1) << 33) - 9000 +
                                    i * 3000 + 1500, output, stdout)) {
            return 3;
        }
    }
    megas_video_free(v);
    return fflush(stdout) != 0;
}
