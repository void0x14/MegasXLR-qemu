/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone fork controller: RFB 3.8 (RFC 6143), no QEMU internals.
 * One persistent connection carries dirty rectangles and input. There is no
 * screenshot loop, video encoder, frame queue, DNS lookup, or worker thread.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "rfb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
typedef SOCKET MgSocket;
#define MG_INVALID INVALID_SOCKET
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
typedef int MgSocket;
#define MG_INVALID (-1)
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MG_RFB_DIM 4096u
#define MG_RFB_PIXELS (9u * 1024u * 1024u)
#define MG_RFB_INPUT 16384u
#define MG_RFB_OUTPUT 8192u
#define MG_RFB_TICK_BYTES (2u * 1024u * 1024u)
#define MG_RFB_HANDSHAKE_MS 1500u
#define MG_RFB_PARTIAL_MS 2000u
#define MG_RFB_TEXT_LIMIT (1024u * 1024u)
#define MG_RFB_METADATA 8192u

enum MgParse {
    MG_MESSAGE, MG_UPDATE, MG_RECTANGLE, MG_RAW, MG_COPY,
    MG_EXT_SIZE, MG_COLOURS, MG_TEXT, MG_SKIP
};
struct MgRfb {
    MgSocket socket;
    uint8_t *pixels;
    unsigned width, height;
    uint64_t generation, last_progress;
    uint8_t input[MG_RFB_INPUT], output[MG_RFB_OUTPUT];
    size_t in_begin, in_end, out_begin, out_end;
    enum MgParse parse;
    unsigned rectangles, x, y, w, h;
    size_t raw_done, raw_pixels, skip;
    unsigned size_reason, size_status;
    int changed, valid, continuous, ext_size;
#ifdef _WIN32
    int winsock;
#endif
};

static void mg_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity) {
        snprintf(error, capacity, "%s", message);
    }
}
static uint16_t mg_get16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}
static uint32_t mg_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void mg_put16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}
static void mg_put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}
static uint64_t mg_milliseconds(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}
static int mg_again(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
           errno == EINPROGRESS;
#endif
}
static void mg_close_socket(MgSocket fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}
static void mg_disconnect(MgRfb *r)
{
    if (r->socket != MG_INVALID) {
        mg_close_socket(r->socket);
        r->socket = MG_INVALID;
    }
    r->valid = 0;
    r->in_begin = r->in_end = r->out_begin = r->out_end = 0;
}
static int mg_nonblock(MgSocket fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 &&
           SetHandleInformation((HANDLE)(uintptr_t)fd, HANDLE_FLAG_INHERIT, 0) ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0), descriptor = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || descriptor < 0 ||
        fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) < 0) { return -1; }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}
/* Only the handshake waits. Every wait shares one deadline. */
static int mg_wait_socket(MgSocket fd, int writing, uint64_t deadline)
{
    for (;;) {
        fd_set set;
        struct timeval wait;
        uint64_t now = mg_milliseconds(), remaining;
        int ready;
        if (now >= deadline) {
            return -1;
        }
        remaining = deadline - now;
        wait.tv_sec = (long)(remaining / 1000u);
        wait.tv_usec = (long)((remaining % 1000u) * 1000u);
        FD_ZERO(&set); FD_SET(fd, &set);
        ready = select((int)fd + 1, writing ? NULL : &set,
                       writing ? &set : NULL, NULL, &wait);
        if (ready > 0) {
            return 0;
        }
        if (ready == 0 || !mg_again()) {
            return -1;
        }
    }
}
static int mg_exact(MgRfb *r, uint8_t *bytes, size_t length, int writing,
                    uint64_t deadline)
{
    size_t offset = 0;
    while (offset < length) {
        int n;
        if (mg_wait_socket(r->socket, writing, deadline) < 0) {
            return -1;
        }
        if (writing) {
            n = (int)send(r->socket, (const char *)bytes + offset,
                          (int)(length - offset), MSG_NOSIGNAL);
        } else {
            n = (int)recv(r->socket, (char *)bytes + offset,
                          (int)(length - offset), 0);
        }
        if (n > 0) {
            offset += (size_t)n;
        } else if (n == 0 || !mg_again()) {
            return -1;
        }
    }
    return 0;
}
static int mg_dimensions(unsigned width, unsigned height)
{
    return width && height && width <= MG_RFB_DIM && height <= MG_RFB_DIM &&
           (size_t)width * height <= MG_RFB_PIXELS;
}
static int mg_set_size(MgRfb *r, unsigned width, unsigned height)
{
    size_t size, i;
    if (!mg_dimensions(width, height)) {
        return -1;
    }
    if (r->pixels && width == r->width && height == r->height) {
        return 0;
    }
    /* Drop the old buffer first: no two full frames during a resize. */
    free(r->pixels); r->pixels = NULL;
    r->width = r->height = 0; r->valid = 0;
    size = (size_t)width * height * 4u;
    r->pixels = calloc(1, size);
    if (!r->pixels) {
        return -1;
    }
    for (i = 3; i < size; i += 4) {
        r->pixels[i] = 255;
    }
    r->width = width; r->height = height;
    r->changed = 1;
    return 0;
}
/* Bounded transmit queue preserves partial writes and exact input ordering. */
static int mg_flush(MgRfb *r)
{
    while (r->out_begin < r->out_end) {
        int n = (int)send(r->socket,
                         (const char *)r->output + r->out_begin,
                         (int)(r->out_end - r->out_begin), MSG_NOSIGNAL);
        if (n > 0) {
            r->out_begin += (size_t)n;
        } else if (n < 0 && mg_again()) {
            return 0;
        } else {
            mg_disconnect(r);
            return -1;
        }
    }
    r->out_begin = r->out_end = 0;
    return 0;
}
static int mg_queue(MgRfb *r, const uint8_t *data, size_t size)
{
    if (!r || r->socket == MG_INVALID || size > sizeof(r->output)) {
        return -1;
    }
    if (sizeof(r->output) - r->out_end < size) {
        if (mg_flush(r) < 0) {
            return -1;
        }
        if (r->out_begin) {
            memmove(r->output, r->output + r->out_begin,
                    r->out_end - r->out_begin);
            r->out_end -= r->out_begin; r->out_begin = 0;
        }
        if (sizeof(r->output) - r->out_end < size) {
            return -1; /* Explicit backpressure, never drop input silently. */
        }
    }
    memcpy(r->output + r->out_end, data, size); r->out_end += size;
    return mg_flush(r);
}
static int mg_update_request(MgRfb *r, int incremental)
{
    uint8_t packet[10] = { 3, 0 };
    packet[1] = (uint8_t)(incremental != 0);
    mg_put16(packet + 6, r->width); mg_put16(packet + 8, r->height);
    return mg_queue(r, packet, sizeof(packet));
}
static int mg_continuous(MgRfb *r)
{
    uint8_t packet[10] = { 150, 1 };
    mg_put16(packet + 6, r->width); mg_put16(packet + 8, r->height);
    return mg_queue(r, packet, sizeof(packet));
}
MgRfb *mg_rfb_new(void)
{
    MgRfb *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->socket = MG_INVALID;
    r->last_progress = mg_milliseconds();
#ifdef _WIN32
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            free(r); return NULL;
        }
        r->winsock = 1;
    }
#endif
    return r;
}
int mg_rfb_connect(MgRfb *r, const char *host, unsigned port,
                   char *error, size_t capacity)
{
    struct sockaddr_in address;
    uint8_t packet[256], init[24];
    uint64_t deadline = mg_milliseconds() + MG_RFB_HANDSHAKE_MS;
    uint32_t length;
    unsigned count, i;
    int result, option = 1, selected = 0;
    mg_error(error, capacity, "");
    if (!r || !host || !port || port > 65535u) {
        mg_error(error, capacity, "Display address is invalid."); return -1;
    }
    mg_disconnect(r);
    r->parse = MG_MESSAGE; r->changed = r->continuous = r->ext_size = 0;
    r->last_progress = mg_milliseconds();
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_port = htons((uint16_t)port);
    if (!strcmp(host, "localhost")) {
        host = "127.0.0.1";
    }
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
        (ntohl(address.sin_addr.s_addr) >> 24) != 127) {
        mg_error(error, capacity, "The display must use a local loopback address.");
        return -1;
    }
    r->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (r->socket == MG_INVALID || mg_nonblock(r->socket) < 0) {
        goto network_error;
    }
#ifndef _WIN32
    if (r->socket >= FD_SETSIZE) {
        mg_error(error, capacity, "Too many open sockets for the display handshake.");
        goto fail;
    }
#endif
    setsockopt(r->socket, IPPROTO_TCP, TCP_NODELAY,
               (const char *)&option, sizeof(option));
#ifdef SO_NOSIGPIPE
    setsockopt(r->socket, SOL_SOCKET, SO_NOSIGPIPE, &option, sizeof(option));
#endif
    result = connect(r->socket, (struct sockaddr *)&address, sizeof(address));
    if (result < 0) {
        int socket_error = 0;
#ifdef _WIN32
        int option_size = sizeof(socket_error);
#else
        socklen_t option_size = sizeof(socket_error);
#endif
        if (!mg_again() || mg_wait_socket(r->socket, 1, deadline) < 0 ||
            getsockopt(r->socket, SOL_SOCKET, SO_ERROR,
                       (char *)&socket_error, &option_size) < 0 || socket_error) {
            goto network_error;
        }
    }
    if (mg_exact(r, packet, 12, 0, deadline) < 0) {
        goto network_error;
    }
    if (memcmp(packet, "RFB 003.008\n", 12)) {
        mg_error(error, capacity, "This display needs RFB 3.8; use the bundled QEMU display.");
        goto fail;
    }
    memcpy(packet, "RFB 003.008\n", 12);
    if (mg_exact(r, packet, 12, 1, deadline) < 0 ||
        mg_exact(r, packet, 1, 0, deadline) < 0) {
        goto network_error;
    }
    count = packet[0];
    if (!count) {
        mg_error(error, capacity, "QEMU refused the display connection; reopen the VM display.");
        goto fail;
    }
    if (mg_exact(r, packet, count, 0, deadline) < 0) {
        goto network_error;
    }
    for (i = 0; i < count; i++) {
        if (packet[i] == 1) { selected = 1; }
    }
    if (!selected) {
        mg_error(error, capacity,
                 "The local display requires a VNC password. Start it with loopback passwordless VNC; guest account passwords are separate.");
        goto fail;
    }
    packet[0] = 1;
    if (mg_exact(r, packet, 1, 1, deadline) < 0 ||
        mg_exact(r, packet, 4, 0, deadline) < 0) {
        goto network_error;
    }
    if (mg_get32(packet)) {
        mg_error(error, capacity, "QEMU rejected local display access; reopen the VM display.");
        goto fail;
    }
    packet[0] = 1; /* Shared: connecting must not evict another viewer. */
    if (mg_exact(r, packet, 1, 1, deadline) < 0 ||
        mg_exact(r, init, sizeof(init), 0, deadline) < 0) {
        goto network_error;
    }
    length = mg_get32(init + 20);
    if (length > MG_RFB_TEXT_LIMIT) {
        mg_error(error, capacity, "The display name exceeds the bounded protocol limit.");
        goto fail;
    }
    while (length) {
        size_t chunk = length > sizeof(packet) ? sizeof(packet) : length;
        if (mg_exact(r, packet, chunk, 0, deadline) < 0) {
            goto network_error;
        }
        length -= (uint32_t)chunk;
    }
    if (mg_set_size(r, mg_get16(init), mg_get16(init + 2)) < 0) {
        mg_error(error, capacity,
                 "The display exceeds the 4096 dimension / 16 MiB framebuffer budget, or memory is unavailable. Choose a smaller resolution.");
        goto fail;
    }
    /* Wire R,G,B,pad: little endian, shifts 0,8,16. GUI gets R,G,B,255. */
    memset(packet, 0, 20); packet[4] = 32; packet[5] = 24; packet[7] = 1;
    mg_put16(packet + 8, 255); mg_put16(packet + 10, 255);
    mg_put16(packet + 12, 255); packet[15] = 8; packet[16] = 16;
    if (mg_queue(r, packet, 20) < 0) { goto network_error; }
    memset(packet, 0, 24); packet[0] = 2; mg_put16(packet + 2, 5);
    mg_put32(packet + 4, 0); mg_put32(packet + 8, 1);
    mg_put32(packet + 12, (uint32_t)-308);
    mg_put32(packet + 16, (uint32_t)-223);
    mg_put32(packet + 20, (uint32_t)-313);
    if (mg_queue(r, packet, 24) < 0 || mg_update_request(r, 0) < 0) {
        goto network_error;
    }
    r->changed = 0; /* Initial blank allocation is not an observed frame. */
    return 0;
network_error:
    mg_error(error, capacity,
             "The live display could not connect within 1.5 seconds. Check the VM process and its local display port; the controller remains available.");
fail:
    mg_disconnect(r); return -1;
}

static int mg_rectangle_done(MgRfb *r)
{
    if (r->rectangles) { r->rectangles--; }
    if (r->rectangles) {
        r->parse = MG_RECTANGLE;
    } else {
        r->parse = MG_MESSAGE;
        if (r->changed) {
            r->generation++; r->changed = 0;
        }
        /* QEMU does not implement ContinuousUpdates. An outstanding native
         * incremental request waits for dirty guest pixels, without polling
         * snapshots or manufacturing frames on a timer. */
        if (!r->continuous && mg_update_request(r, 1) < 0) { return -1; }
    }
    return 0;
}
static int mg_bounds(MgRfb *r, unsigned x, unsigned y, unsigned w, unsigned h)
{
    return x <= r->width && y <= r->height &&
           w <= r->width - x && h <= r->height - y;
}
/* Consume as much of the bounded input window as possible. Return -2 for a
 * rejected guest resize, -1 for an invalid/disconnected protocol, 0 otherwise.
 */
static int mg_parse(MgRfb *r, char *error, size_t capacity)
{
    while (r->in_begin < r->in_end) {
        const uint8_t *p = r->input + r->in_begin;
        size_t available = r->in_end - r->in_begin, use = 0;
        switch (r->parse) {
        case MG_MESSAGE:
            use = 1;
            switch (p[0]) {
            case 0: r->parse = MG_UPDATE; break;
            case 1: r->parse = MG_COLOURS; break;
            case 2: break; /* Bell has no payload. */
            case 3: r->parse = MG_TEXT; break;
            case 150:
                r->continuous = 1;
                if (mg_continuous(r) < 0) { goto disconnected; }
                break;
            default:
                mg_error(error, capacity, "The display sent an unsupported server message; reconnect the VM display.");
                return -1;
            }
            break;
        case MG_UPDATE:
            if (available < 3) { return 0; }
            use = 3; r->rectangles = mg_get16(p + 1);
            r->parse = MG_RECTANGLE;
            if (!r->rectangles && mg_rectangle_done(r) < 0) {
                goto disconnected;
            }
            break;
        case MG_RECTANGLE:
        {
            int32_t encoding;
            if (available < 12) { return 0; }
            use = 12;
            r->x = mg_get16(p); r->y = mg_get16(p + 2);
            r->w = mg_get16(p + 4); r->h = mg_get16(p + 6);
            encoding = (int32_t)mg_get32(p + 8);
            if (encoding == -223) {
                if (r->x || r->y || mg_set_size(r, r->w, r->h) < 0) {
                    goto invalid_size;
                }
                if (r->continuous && mg_continuous(r) < 0) {
                    goto disconnected;
                }
                if (mg_rectangle_done(r) < 0) { goto disconnected; }
            } else if (encoding == -308) {
                r->ext_size = 1;
                r->size_reason = r->x; r->size_status = r->y;
                r->parse = MG_EXT_SIZE;
            } else if (encoding == 0 || encoding == 1) {
                if (!mg_bounds(r, r->x, r->y, r->w, r->h)) {
                    mg_error(error, capacity, "The display rectangle lies outside its framebuffer; reconnect the VM display.");
                    return -1;
                }
                if (encoding == 1) {
                    r->parse = MG_COPY;
                } else {
                    r->raw_done = 0;
                    r->raw_pixels = (size_t)r->w * r->h;
                    r->parse = MG_RAW;
                    if (!r->raw_pixels && mg_rectangle_done(r) < 0) {
                        goto disconnected;
                    }
                }
            } else {
                mg_error(error, capacity, "The display sent an encoding it did not negotiate; reconnect the VM display.");
                return -1;
            }
            break;
        }
        case MG_RAW:
        {
            size_t count = available / 4u;
            if (!count) { return 0; }
            if (count > r->raw_pixels - r->raw_done) {
                count = r->raw_pixels - r->raw_done;
            }
            use = count * 4u;
            while (count) {
                size_t row = r->raw_done / r->w, column = r->raw_done % r->w;
                size_t part = r->w - column, i;
                uint8_t *destination;
                if (part > count) { part = count; }
                destination = r->pixels + (((size_t)r->y + row) * r->width + r->x + column) * 4u;
                memcpy(destination, p, part * 4u);
                for (i = 3; i < part * 4u; i += 4u) { destination[i] = 255; }
                r->raw_done += part; p += part * 4u; count -= part;
            }
            r->changed = 1;
            if (r->raw_done == r->raw_pixels) {
                r->valid = 1;
                if (mg_rectangle_done(r) < 0) { goto disconnected; }
            }
            break;
        }
        case MG_COPY:
        {
            unsigned source_x, source_y, row;
            if (available < 4) { return 0; }
            use = 4; source_x = mg_get16(p); source_y = mg_get16(p + 2);
            if (!mg_bounds(r, source_x, source_y, r->w, r->h)) {
                mg_error(error, capacity, "The display copy source lies outside its framebuffer; reconnect the VM display.");
                return -1;
            }
            /* Bottom-to-top when the destination overlaps below the source. */
            for (row = 0; row < r->h; row++) {
                unsigned j = r->y > source_y ? r->h - 1u - row : row;
                memmove(r->pixels + ((size_t)(r->y + j) * r->width + r->x) * 4u,
                        r->pixels + ((size_t)(source_y + j) * r->width + source_x) * 4u,
                        (size_t)r->w * 4u);
            }
            r->changed = 1;
            if (mg_rectangle_done(r) < 0) { goto disconnected; }
            break;
        }
        case MG_EXT_SIZE:
        {
            unsigned screens, i;
            int rejected;
            if (available < 4) { return 0; }
            screens = p[0]; use = 4u + (size_t)screens * 16u;
            if (available < use) { return 0; }
            rejected = r->size_reason == 1 && r->size_status != 0;
            if (!rejected) {
                if (!screens || !mg_dimensions(r->w, r->h)) { goto invalid_size; }
                for (i = 0; i < screens; i++) {
                    const uint8_t *screen = p + 4u + (size_t)i * 16u;
                    unsigned x = mg_get16(screen + 4), y = mg_get16(screen + 6);
                    unsigned w = mg_get16(screen + 8), h = mg_get16(screen + 10);
                    if (!w || !h || x > r->w || y > r->h ||
                        w > r->w - x || h > r->h - y) {
                        goto invalid_size;
                    }
                }
                if (mg_set_size(r, r->w, r->h) < 0) { goto invalid_size; }
                if (r->continuous && mg_continuous(r) < 0) { goto disconnected; }
            }
            if (mg_rectangle_done(r) < 0) { goto disconnected; }
            if (rejected && r->size_status != 4) {
                r->in_begin += use;
                mg_error(error, capacity,
                         "The guest display rejected the resolution change. A supported guest display driver is required; its current resolution is unchanged.");
                return -2;
            }
            /* QEMU status 4 means forwarded to the guest, not resized. */
            break;
        }
        case MG_COLOURS:
            if (available < 5) { return 0; }
            use = 5; r->skip = (size_t)mg_get16(p + 3) * 6u;
            r->parse = r->skip ? MG_SKIP : MG_MESSAGE;
            break;
        case MG_TEXT:
            if (available < 7) { return 0; }
            use = 7; r->skip = mg_get32(p + 3);
            if (r->skip > MG_RFB_TEXT_LIMIT) {
                mg_error(error, capacity, "The display clipboard exceeds the bounded protocol limit; reconnect the VM display.");
                return -1;
            }
            r->parse = r->skip ? MG_SKIP : MG_MESSAGE;
            break;
        case MG_SKIP:
            use = available < r->skip ? available : r->skip;
            r->skip -= use;
            if (!r->skip) { r->parse = MG_MESSAGE; }
            break;
        }
        r->in_begin += use;
    }
    return 0;
invalid_size:
    mg_error(error, capacity,
             "The guest display layout exceeds the 4096 dimension / 16 MiB framebuffer limit, is invalid, or memory is unavailable. Choose a smaller resolution.");
    return -1;
disconnected:
    mg_error(error, capacity, "The live display disconnected; reconnect it before sending input.");
    return -1;
}
int mg_rfb_tick(MgRfb *r, char *error, size_t capacity)
{
    size_t received = 0;
    uint64_t started = mg_milliseconds(), generation;
    unsigned iterations = 0;
    mg_error(error, capacity, "");
    if (!r || r->socket == MG_INVALID) {
        mg_error(error, capacity, "The live display is disconnected."); return -1;
    }
    generation = r->generation;
    if (mg_flush(r) < 0) { goto disconnected; }
    while (received < MG_RFB_TICK_BYTES && iterations++ < 2048u &&
           mg_milliseconds() - started < 8u) {
        int result = mg_parse(r, error, capacity), n;
        if (result < 0) {
            if (result == -1) { mg_disconnect(r); }
            return result;
        }
        if (r->in_begin) {
            memmove(r->input, r->input + r->in_begin, r->in_end - r->in_begin);
            r->in_end -= r->in_begin; r->in_begin = 0;
        }
        if (r->in_end == sizeof(r->input)) {
            mg_error(error, capacity, "The display parser exceeded its bounded input window.");
            mg_disconnect(r); return -1;
        }
        n = (int)recv(r->socket, (char *)r->input + r->in_end,
                      (int)(sizeof(r->input) - r->in_end), 0);
        if (n > 0) {
            r->in_end += (size_t)n; received += (size_t)n;
            r->last_progress = mg_milliseconds();
        } else if (n < 0 && mg_again()) {
            break;
        } else {
            goto disconnected;
        }
    }
    /* A quiet complete desktop is healthy. An abandoned partial update is
     * not: keep the owner responsive and stop presenting its picture as live. */
    if ((r->parse != MG_MESSAGE || r->in_end != r->in_begin) &&
        mg_milliseconds() - r->last_progress >= MG_RFB_PARTIAL_MS) {
        mg_disconnect(r);
        mg_error(error, capacity,
                 "The live display stopped partway through an update. Reconnect its local display; the controller remains available.");
        return -1;
    }
    return r->generation != generation ? 1 : 0;
disconnected:
    mg_disconnect(r);
    mg_error(error, capacity,
             "The live display disconnected. The last picture is no longer live; reconnect the VM display.");
    return -1;
}
const uint8_t *mg_rfb_pixels(MgRfb *r, unsigned *width, unsigned *height,
                             uint64_t *generation)
{
    if (width) { *width = r ? r->width : 0; }
    if (height) { *height = r ? r->height : 0; }
    if (generation) { *generation = r ? r->generation : 0; }
    return r && r->socket != MG_INVALID && r->valid && r->generation ? r->pixels : NULL;
}
size_t mg_rfb_pending(MgRfb *r)
{
    return r && r->socket != MG_INVALID ? r->out_end - r->out_begin : 0;
}
int mg_rfb_flush(MgRfb *r, unsigned timeout_ms, char *error, size_t capacity)
{
    uint64_t deadline;
    mg_error(error, capacity, "");
    if (!r || r->socket == MG_INVALID) {
        mg_error(error, capacity, "The live display is disconnected; input delivery cannot be confirmed.");
        return -1;
    }
    /* Even an accidental huge caller timeout cannot freeze the owner thread. */
    if (timeout_ms > 300u) { timeout_ms = 300u; }
    deadline = mg_milliseconds() + timeout_ms;
    for (;;) {
        if (mg_flush(r) < 0) {
            mg_error(error, capacity, "The live display disconnected before queued input was delivered.");
            return -1;
        }
        if (!mg_rfb_pending(r)) { return 0; }
        if (mg_wait_socket(r->socket, 1, deadline) < 0) {
            mg_error(error, capacity,
                     "Display input is still queued. Do not repeat the accepted action; let the live display recover before taking its next checkpoint.");
            return -2;
        }
    }
    /* A drained TCP queue confirms transport delivery only. RFB has no input
     * acknowledgment and cannot prove guest application completion. */
}

int mg_rfb_key(MgRfb *r, uint32_t keysym, int down)
{
    uint8_t packet[8] = { 4, 0 };
    packet[1] = (uint8_t)(down != 0); mg_put32(packet + 4, keysym);
    return mg_queue(r, packet, sizeof(packet));
}
int mg_rfb_pointer(MgRfb *r, unsigned x, unsigned y, unsigned buttons)
{
    uint8_t packet[6] = { 5, 0 };
    if (!r || !r->width || !r->height || buttons > 255u) { return -1; }
    if (x >= r->width) { x = r->width - 1u; }
    if (y >= r->height) { y = r->height - 1u; }
    packet[1] = (uint8_t)buttons;
    mg_put16(packet + 2, x); mg_put16(packet + 4, y);
    return mg_queue(r, packet, sizeof(packet));
}
int mg_rfb_resize(MgRfb *r, unsigned width, unsigned height)
{
    uint8_t packet[24] = { 251, 0 };
    if (!r || !r->ext_size || !mg_dimensions(width, height)) { return -1; }
    mg_put16(packet + 2, width); mg_put16(packet + 4, height); packet[6] = 1;
    /* One screen, id 0, origin (0,0), matching QEMU's single display. */
    mg_put16(packet + 16, width); mg_put16(packet + 18, height);
    return mg_queue(r, packet, sizeof(packet));
}

/* PNG: one scanline of scratch memory, stored DEFLATE, streaming IDAT chunks.
 * Metadata is observed controller state, not OCR or invented screen content.
 */
static uint32_t mg_crc32(uint32_t crc, const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++) {
        unsigned j;
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc;
}
static int mg_png_chunk(FILE *file, const char *type,
                        const uint8_t *data, size_t size)
{
    uint8_t integer[4];
    uint32_t crc;
    if (size > UINT32_MAX) { return -1; }
    mg_put32(integer, (uint32_t)size);
    if (fwrite(integer, 1, 4, file) != 4 || fwrite(type, 1, 4, file) != 4 ||
        (size && fwrite(data, 1, size, file) != size)) { return -1; }
    crc = mg_crc32(0xffffffffu, (const uint8_t *)type, 4);
    crc = mg_crc32(crc, data, size) ^ 0xffffffffu;
    mg_put32(integer, crc);
    return fwrite(integer, 1, 4, file) == 4 ? 0 : -1;
}
/* Write beside the target so replacement stays on one filesystem. O_EXCL
 * prevents clobbering or following another process's temporary file. */
#define MG_PNG_PATH 1200u
#ifdef _WIN32
static int mg_wide_path(const char *path, wchar_t *wide)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                              wide, MG_PNG_PATH) ? 0 : -1;
}
#endif
static void mg_png_remove(const char *path)
{
#ifdef _WIN32
    wchar_t wide[MG_PNG_PATH];
    if (mg_wide_path(path, wide) == 0) { _wremove(wide); }
#else
    remove(path);
#endif
}
static FILE *mg_png_temporary(const char *path, char *temporary, size_t capacity)
{
    static unsigned sequence;
    unsigned attempt;
    unsigned long pid;
#ifdef _WIN32
    pid = GetCurrentProcessId();
#else
    pid = (unsigned long)getpid();
#endif
    for (attempt = 0; attempt < 8u; attempt++) {
        int fd, count = snprintf(temporary, capacity, "%s.%lu.%u.part",
                                 path, pid, ++sequence);
        FILE *file;
        if (count < 0 || (size_t)count >= capacity) { return NULL; }
#ifdef _WIN32
        {
            wchar_t wide[MG_PNG_PATH];
            if (mg_wide_path(temporary, wide) < 0) { return NULL; }
            fd = _wopen(wide, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                         _S_IREAD | _S_IWRITE);
        }
#else
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
        if (fd < 0) {
            if (errno == EEXIST) { continue; }
            return NULL;
        }
#ifdef _WIN32
        file = _wfdopen(fd, L"wb");
        if (!file) { _close(fd); }
#else
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            close(fd); mg_png_remove(temporary); return NULL;
        }
        file = fdopen(fd, "wb");
        if (!file) { close(fd); }
#endif
        if (!file) { mg_png_remove(temporary); }
        return file;
    }
    return NULL;
}
static int mg_png_replace(const char *temporary, const char *target)
{
#ifdef _WIN32
    wchar_t source[MG_PNG_PATH], destination[MG_PNG_PATH];
    if (mg_wide_path(temporary, source) < 0 || mg_wide_path(target, destination) < 0) {
        return -1;
    }
    /* Unlike _wrename, this replaces an existing target in one operation.
     * A reader without delete sharing may prevent replacement; keep its old
     * image intact and report failure rather than deleting it first. */
    return MoveFileExW(source, destination,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(temporary, target);
#endif
}

int mg_rfb_png(MgRfb *r, const char *path, const char *metadata,
               char *error, size_t capacity)
{
    static const uint8_t signature[8] = { 137,80,78,71,13,10,26,10 };
    uint8_t header[13] = { 0 }, *row = NULL, tail[4], zlib_header[2] = { 0x78, 0x01 };
    FILE *file = NULL;
    char temporary[MG_PNG_PATH] = { 0 };
    unsigned y, x;
    size_t row_size, metadata_size;
    uint32_t a = 1, b = 0;
    int result = -1;
    mg_error(error, capacity, "");
    if (!r || r->socket == MG_INVALID || !r->valid || !r->generation || !path || !*path) {
        mg_error(error, capacity, "A current live display frame is required before exporting its picture.");
        return -1;
    }
    if (r->parse == MG_RAW || r->parse == MG_COPY || r->parse == MG_RECTANGLE) {
        mg_error(error, capacity, "The live display is completing an update. Export its picture after the next display tick.");
        return -1;
    }
    row_size = 1u + (size_t)r->width * 3u;
    row = malloc(row_size + 5u);
    if (!row) { mg_error(error, capacity, "Memory is unavailable for one picture scanline."); return -1; }
    file = mg_png_temporary(path, temporary, sizeof(temporary));
    if (!file) { mg_error(error, capacity, "The picture location is not writable. Choose a writable folder."); goto done; }
    if (fwrite(signature, 1, sizeof(signature), file) != sizeof(signature)) { goto write_error; }
    mg_put32(header, r->width); mg_put32(header + 4, r->height);
    header[8] = 8; header[9] = 2; /* 8-bit RGB, no interlace. */
    if (mg_png_chunk(file, "IHDR", header, sizeof(header)) < 0) { goto write_error; }
    metadata_size = 0;
    while (metadata && metadata_size < MG_RFB_METADATA && metadata[metadata_size]) {
        metadata_size++;
    }
    if (metadata_size) {
        uint8_t *text = malloc(metadata_size + 17u);
        if (!text) { mg_error(error, capacity, "Memory is unavailable for picture state metadata."); goto done; }
        /* iTXt keyword, compression flag/method, language, translated keyword. */
        memcpy(text, "Megas-State", 11); memset(text + 11, 0, 5);
        memcpy(text + 16, metadata, metadata_size);
        if (mg_png_chunk(file, "iTXt", text, metadata_size + 16u) < 0) {
            free(text); goto write_error;
        }
        free(text);
    }
    if (mg_png_chunk(file, "IDAT", zlib_header, sizeof(zlib_header)) < 0) { goto write_error; }
    for (y = 0; y < r->height; y++) {
        const uint8_t *source = r->pixels + (size_t)y * r->width * 4u;
        row[0] = (uint8_t)(y + 1u == r->height); /* BFINAL, uncompressed block. */
        row[1] = (uint8_t)row_size; row[2] = (uint8_t)(row_size >> 8);
        row[3] = (uint8_t)~row_size; row[4] = (uint8_t)(~row_size >> 8);
        row[5] = 0; /* PNG filter None. */
        for (x = 0; x < r->width; x++) {
            memcpy(row + 6u + (size_t)x * 3u, source + (size_t)x * 4u, 3);
        }
        for (x = 0; x < row_size; x++) {
            a = (a + row[5u + x]) % 65521u; b = (b + a) % 65521u;
        }
        if (mg_png_chunk(file, "IDAT", row, row_size + 5u) < 0) { goto write_error; }
    }
    mg_put32(tail, (b << 16) | a);
    if (mg_png_chunk(file, "IDAT", tail, sizeof(tail)) < 0 ||
        mg_png_chunk(file, "IEND", NULL, 0) < 0) { goto write_error; }
    result = 0;
    goto done;
write_error:
    mg_error(error, capacity, "The picture could not be written completely. Free disk space or choose another folder.");
done:
    if (file && fclose(file) != 0) {
        result = -1; mg_error(error, capacity, "The picture could not be saved completely. Check available disk space.");
    }
    if (result == 0 && mg_png_replace(temporary, path) < 0) {
        result = -1;
        mg_error(error, capacity,
                 "The new picture could not replace the previous one. Its previous image is intact; choose a writable location or close a file reader.");
    }
    if (result < 0 && file) { mg_png_remove(temporary); }
    free(row); return result;
}
void mg_rfb_free(MgRfb *r)
{
    if (!r) { return; }
    mg_disconnect(r); free(r->pixels);
#ifdef _WIN32
    if (r->winsock) { WSACleanup(); }
#endif
    free(r);
}
