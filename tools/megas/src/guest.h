/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEGAS_GUEST_H
#define MEGAS_GUEST_H
#include "platform.h"
#include <stddef.h>
#include <stdint.h>
#define MG_GUEST_MESSAGE_CAP 65536
#define MG_GUEST_PAYLOAD_CAP 24576
/* Owner-thread job. Password storage is private to the bridge and is wiped
 * before completion/failure; never serialize this struct or log its secrets. */
typedef struct {
    unsigned port, stage, kind;
    uint64_t pid;
    int active, uncertain, windows, enabled, expected_os;
    char username[33], password[257], helpers[1024], guest_path[256], wad[1024];
    char progress[512];
    MgpSocket socket;
    uint64_t request_id, deadline, next_poll, file_handle, sync_nonce;
    size_t tx_used, tx_sent, rx_used, payload_used, payload_sent;
    int awaiting, synced, sync_sentinel, file_open, password_pending;
    char tx[MG_GUEST_MESSAGE_CAP], rx[MG_GUEST_MESSAGE_CAP];
    unsigned char payload[MG_GUEST_PAYLOAD_CAP];
} MgGuestJob;
/* Start returns 0 only for a queued identity check, never completed policy. */
int mg_guest_policy_start(MgGuestJob *, unsigned port, const char *os,
                          int enabled, const char *username, const char *password,
                          const char *helper_dir, char *error, size_t capacity);
int mg_guest_doom_start(MgGuestJob *, unsigned port, const char *helper_dir,
                        const char *wad, char *error, size_t capacity);
/* Poll: 0 still pending, 1 confirmed completion, -1 failed/unconfirmed.
 * QGA captures stdout only at exit; running feedback names its actual PID. */
int mg_guest_poll(MgGuestJob *, char *error, size_t capacity);
void mg_guest_clear(MgGuestJob *);
#endif
