/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEGAS_LIVE_H
#define MEGAS_LIVE_H
#include "platform.h"

#define MG_LIVE_LINE 24576
#define MG_LIVE_CHUNK (188 * 128)
typedef struct MgLive MgLive;
/* No workers, encoder, screenshots, private provider code or disk spool.
 * Inputs are QEMU-owned loopback virtserialport backends. */
MgLive *mg_live_new(unsigned control, unsigned video, unsigned http,
                    const char *credential);
void mg_live_free(MgLive *);
void mg_live_tick(MgLive *);
/* Returned line is borrowed until the next tick. Empty means no new message. */
const char *mg_live_line(MgLive *);
int mg_live_send(MgLive *, const char *line);
void mg_live_abort_control(MgLive *);
int mg_live_control_up(MgLive *);
int mg_live_video_up(MgLive *);
uint64_t mg_live_generation(MgLive *);
uint64_t mg_live_bytes(MgLive *);
uint64_t mg_live_slow_clients(MgLive *);
uint64_t mg_live_video_age(MgLive *);
#endif
