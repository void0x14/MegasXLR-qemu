/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "live.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif

#define CLIENTS 4
typedef struct {
    MgpSocket socket;
    uint64_t accepted, progress;
    int streaming;
    size_t used, sent;
    char request[4096];
    size_t request_used;
    unsigned char output[MG_LIVE_CHUNK + 512];
} Viewer;
struct MgLive {
    unsigned control_port, video_port;
    MgpSocket control, video, listener;
    uint64_t retry_control, retry_video, generation, bytes, slow, last_video;
    char credential[65];
    char rx[MG_LIVE_LINE], line[MG_LIVE_LINE], tx[8192];
    size_t rx_used, tx_used, tx_sent;
    unsigned char video_data[MG_LIVE_CHUNK + 188];
    size_t video_used;
    Viewer viewers[CLIENTS];
};

static void viewer_close(Viewer *v)
{
    mgp_close(v->socket);
    memset(v, 0, sizeof(*v));
    v->socket = MGP_BAD_SOCKET;
}
static void video_close(MgLive *s)
{
    unsigned i;
    mgp_close(s->video); s->video = MGP_BAD_SOCKET;
    s->video_used = 0; s->last_video = 0;
    for (i = 0; i < CLIENTS; i++) viewer_close(&s->viewers[i]);
}
static void control_close(MgLive *s)
{
    mgp_close(s->control); s->control = MGP_BAD_SOCKET;
    s->rx_used = s->tx_used = s->tx_sent = 0;
    s->generation++;
}
MgLive *mg_live_new(unsigned control, unsigned video, unsigned http,
                    const char *credential)
{
    MgLive *s;
    unsigned i;
    if (!control || !video || !http || control > 65535 || video > 65535 ||
        http > 65535 || control == video || control == http || video == http ||
        !credential || strlen(credential) != 64) return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->control = s->video = s->listener = MGP_BAD_SOCKET;
    for (i = 0; i < CLIENTS; i++) s->viewers[i].socket = MGP_BAD_SOCKET;
    s->control_port = control; s->video_port = video;
    memcpy(s->credential, credential, 65);
    s->listener = mgp_listen(http);
    if (s->listener == MGP_BAD_SOCKET) { mg_live_free(s); return NULL; }
    return s;
}
void mg_live_free(MgLive *s)
{
    if (!s) return;
    video_close(s); control_close(s); mgp_close(s->listener);
    memset(s->credential, 0, sizeof(s->credential));
    free(s);
}

static int authorized(MgLive *s, const char *request)
{
    const char *p = strstr(request, "\r\nAuthorization: Bearer ");
    unsigned mismatch = 0, i;
    if (!p) return 0;
    p += strlen("\r\nAuthorization: Bearer ");
    if (strlen(p) < 66 || p[64] != '\r' || p[65] != '\n') return 0;
    for (i = 0; i < 64; i++) mismatch |= (unsigned char)p[i] ^ (unsigned char)s->credential[i];
    return mismatch == 0;
}
static void viewer_tick(MgLive *s, Viewer *v, uint64_t now)
{
    int n;
    if (v->socket == MGP_BAD_SOCKET) return;
    if (!v->streaming) {
        n = mgp_recv(v->socket, v->request + v->request_used,
                     sizeof(v->request) - v->request_used - 1);
        if (n == 0 || n == -1) { viewer_close(v); return; }
        if (n > 0) {
            v->request_used += (size_t)n;
            v->request[v->request_used] = 0;
            if (strstr(v->request, "\r\n\r\n")) {
                const char *header;
                int ok = !strncmp(v->request, "GET /live.ts HTTP/1.", 20) && authorized(s, v->request);
                if (!ok) header = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                else if (!s->last_video) header = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                else header = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
                v->used = strlen(header); memcpy(v->output, header, v->used);
                v->streaming = ok && s->last_video ? 1 : -1;
                memset(v->request, 0, sizeof(v->request));
            }
        }
        if (!v->streaming && (v->request_used == sizeof(v->request) - 1 || now - v->accepted >= 2000)) {
            viewer_close(v); return;
        }
    }
    if (v->used > v->sent) {
        n = mgp_send(v->socket, v->output + v->sent, v->used - v->sent);
        if (n == -1 || n == 0) { viewer_close(v); return; }
        if (n > 0) { v->sent += (size_t)n; v->progress = now; }
        if (v->sent == v->used) {
            v->used = v->sent = 0;
            if (v->streaming < 0) viewer_close(v);
        } else if (now - v->progress >= 500) {
            s->slow++; viewer_close(v);
        }
    }
}

void mg_live_tick(MgLive *s)
{
    uint64_t now;
    unsigned i;
    int n;
    size_t complete;
    char *newline;
    if (!s) return;
    now = mgp_now(); s->line[0] = 0;
    if (s->control == MGP_BAD_SOCKET && now >= s->retry_control) {
        s->retry_control = now + 250;
        s->control = mgp_connect(s->control_port, 1);
        if (s->control != MGP_BAD_SOCKET) s->generation++;
    }
    if (s->control != MGP_BAD_SOCKET) {
        if (s->tx_sent < s->tx_used) {
            n = mgp_send(s->control, s->tx + s->tx_sent, s->tx_used - s->tx_sent);
            if (n == -1 || n == 0) control_close(s);
            else if (n > 0) s->tx_sent += (size_t)n;
            if (s->tx_used == s->tx_sent) s->tx_used = s->tx_sent = 0;
        }
        if (s->control != MGP_BAD_SOCKET) {
            newline = memchr(s->rx, '\n', s->rx_used);
            if (!newline) {
                n = mgp_recv(s->control, s->rx + s->rx_used, sizeof(s->rx) - s->rx_used - 1);
                if (n == 0 || n == -1) control_close(s);
                else if (n > 0) s->rx_used += (size_t)n;
                newline = memchr(s->rx, '\n', s->rx_used);
            }
            if (newline) {
                size_t length = (size_t)(newline - s->rx), consumed = length + 1;
                if (memchr(s->rx, 0, length)) control_close(s);
                else {
                    memcpy(s->line, s->rx, length); s->line[length] = 0;
                    memmove(s->rx, s->rx + consumed, s->rx_used - consumed);
                    s->rx_used -= consumed;
                }
            } else if (s->rx_used == sizeof(s->rx) - 1) control_close(s);
        }
    }
    for (i = 0; i < CLIENTS; i++) viewer_tick(s, &s->viewers[i], now);
    if (mgp_ready(s->listener, 0, 0) > 0) {
        MgpSocket incoming = accept(s->listener, NULL, NULL);
        if (incoming != MGP_BAD_SOCKET) {
            for (i = 0; i < CLIENTS; i++) if (s->viewers[i].socket == MGP_BAD_SOCKET) break;
            if (i == CLIENTS || mgp_nonblock(incoming)) mgp_close(incoming);
            else { s->viewers[i].socket = incoming; s->viewers[i].accepted = s->viewers[i].progress = now; }
        }
    }
    if (s->video == MGP_BAD_SOCKET && now >= s->retry_video) {
        s->retry_video = now + 250;
        s->video = mgp_connect(s->video_port, 1);
    }
    if (s->video == MGP_BAD_SOCKET) return;
    n = mgp_recv(s->video, s->video_data + s->video_used, MG_LIVE_CHUNK - s->video_used);
    if (n == 0 || n == -1) { video_close(s); return; }
    if (n <= 0) return;
    s->video_used += (size_t)n;
    complete = s->video_used / 188 * 188;
    if (!complete) return;
    for (i = 0; i < complete; i += 188) if (s->video_data[i] != 0x47) { video_close(s); return; }
    s->last_video = now; s->bytes += complete;
    for (i = 0; i < CLIENTS; i++) {
        Viewer *v = &s->viewers[i];
        if (v->socket == MGP_BAD_SOCKET || v->streaming != 1) continue;
        if (v->used + complete > sizeof(v->output)) { s->slow++; viewer_close(v); continue; }
        if (!v->used) v->progress = now;
        memcpy(v->output + v->used, s->video_data, complete); v->used += complete;
    }
    memmove(s->video_data, s->video_data + complete, s->video_used - complete);
    s->video_used -= complete;
}
const char *mg_live_line(MgLive *s) { return s ? s->line : ""; }
int mg_live_send(MgLive *s, const char *line)
{
    size_t n;
    if (!s || !line || s->control == MGP_BAD_SOCKET || s->tx_used) return -1;
    n = strlen(line);
    if (n + 1 >= sizeof(s->tx) || strchr(line, '\n')) return -1;
    memcpy(s->tx, line, n); s->tx[n] = '\n'; s->tx_used = n + 1;
    return 0;
}
void mg_live_abort_control(MgLive *s) { if(s) control_close(s); }
int mg_live_control_up(MgLive *s) { return s && s->control != MGP_BAD_SOCKET; }
int mg_live_video_up(MgLive *s) { return s && s->video != MGP_BAD_SOCKET && s->last_video && mgp_now() - s->last_video < 2000; }
uint64_t mg_live_generation(MgLive *s) { return s ? s->generation : 0; }
uint64_t mg_live_bytes(MgLive *s) { return s ? s->bytes : 0; }
uint64_t mg_live_slow_clients(MgLive *s) { return s ? s->slow : 0; }
uint64_t mg_live_video_age(MgLive *s) { return s && s->last_video ? mgp_now() - s->last_video : 0; }
