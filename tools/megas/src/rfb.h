/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEGAS_RFB_H
#define MEGAS_RFB_H
#include <stdint.h>
#include <stddef.h>
typedef struct MgRfb MgRfb;
MgRfb *mg_rfb_new(void);
int mg_rfb_connect(MgRfb *, const char *host, unsigned port, char *error, size_t cap);
/* Nonblocking after the bounded handshake. Latest frame only, no frame queue. */
int mg_rfb_tick(MgRfb *, char *error, size_t cap);
const uint8_t *mg_rfb_pixels(MgRfb *, unsigned *width, unsigned *height, uint64_t *generation);
int mg_rfb_key(MgRfb *, uint32_t keysym, int down);
int mg_rfb_pointer(MgRfb *, unsigned x, unsigned y, unsigned buttons);
int mg_rfb_resize(MgRfb *, unsigned width, unsigned height);
int mg_rfb_flush(MgRfb *, unsigned timeout_ms, char *error, size_t cap);
size_t mg_rfb_pending(MgRfb *);
int mg_rfb_png(MgRfb *, const char *path, const char *metadata, char *error, size_t cap);
void mg_rfb_free(MgRfb *);
#endif
