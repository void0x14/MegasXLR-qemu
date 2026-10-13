/* SPDX-License-Identifier: GPL-2.0-or-later */
/* White-box protocol tests use the real nonblocking parser and socket writes.
 * Fragmentation, bounds, disconnect, copy overlap, resize rejection and PNG
 * checks target behavior that a happy-path screenshot test cannot exercise.
 */
#include "../src/rfb.c"
#include <assert.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <dirent.h>

static MgRfb *test_display(int *peer, unsigned w, unsigned h)
{
    int sockets[2];
    MgRfb *r = mg_rfb_new();
    assert(r && socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    r->socket = sockets[0]; *peer = sockets[1];
    assert(mg_nonblock(r->socket) == 0);
    assert((fcntl(r->socket, F_GETFD) & FD_CLOEXEC) != 0);
    assert(mg_nonblock(*peer) == 0);
    assert(mg_set_size(r, w, h) == 0);
    r->changed = 0;
    return r;
}
static void inject(MgRfb *r, int peer, const uint8_t *data, size_t size,
                    size_t fragment)
{
    char error[256];
    size_t i = 0;
    while (i < size) {
        size_t n = size - i < fragment ? size - i : fragment;
        assert(send(peer, data + i, n, MSG_NOSIGNAL) == (ssize_t)n);
        assert(mg_rfb_tick(r, error, sizeof(error)) >= 0);
        i += n;
    }
}
static size_t rect(uint8_t *p, unsigned x, unsigned y, unsigned w, unsigned h,
                    int32_t encoding)
{
    p[0] = p[1] = 0; mg_put16(p + 2, 1);
    mg_put16(p + 4, x); mg_put16(p + 6, y);
    mg_put16(p + 8, w); mg_put16(p + 10, h);
    mg_put32(p + 12, (uint32_t)encoding);
    return 16;
}
static void raw_and_input(void)
{
    int peer;
    MgRfb *r = test_display(&peer, 4, 3);
    uint8_t packet[256], reply[256];
    const uint8_t *pixels;
    size_t n = rect(packet, 0, 0, 4, 3, 0), i;
    unsigned w, h;
    uint64_t generation;
    for (i = 0; i < 12; i++) {
        packet[n++] = (uint8_t)i; packet[n++] = 42;
        packet[n++] = 101; packet[n++] = 0;
    }
    inject(r, peer, packet, n, 1); /* Pixel split in all four byte positions. */
    pixels = mg_rfb_pixels(r, &w, &h, &generation);
    assert(pixels && w == 4 && h == 3 && generation == 1);
    for (i = 0; i < 12; i++) {
        assert(pixels[i * 4] == i && pixels[i * 4 + 1] == 42);
        assert(pixels[i * 4 + 2] == 101 && pixels[i * 4 + 3] == 255);
    }
    assert(recv(peer, reply, sizeof(reply), 0) == 10);
    assert(reply[0] == 3 && reply[1] == 1 && mg_get16(reply + 6) == 4);
    assert(mg_rfb_key(r, 0xff0d, 1) == 0);
    assert(mg_rfb_key(r, 0xff0d, 0) == 0);
    assert(mg_rfb_pointer(r, 999, 999, 1) == 0);
    assert(recv(peer, reply, sizeof(reply), 0) == 22);
    assert(reply[0] == 4 && reply[1] == 1 && mg_get32(reply + 4) == 0xff0d);
    assert(reply[8] == 4 && reply[9] == 0);
    assert(reply[16] == 5 && reply[17] == 1);
    assert(mg_get16(reply + 18) == 3 && mg_get16(reply + 20) == 2);
    /* Overlapping downward copy must preserve the source before overwrite. */
    n = rect(packet, 0, 1, 4, 2, 1);
    mg_put16(packet + n, 0); mg_put16(packet + n + 2, 0);
    inject(r, peer, packet, n + 4, 3);
    pixels = mg_rfb_pixels(r, NULL, NULL, &generation);
    assert(generation == 2);
    for (i = 0; i < 8; i++) { assert(pixels[(i + 4) * 4] == i); }
    assert(mg_rfb_png(r, "/tmp/megas-rfb-test.png", "guest=running; observed=true; Türkçe", NULL, 0) == 0);
    close(peer); mg_rfb_free(r);
}
static void png_atomic_replacement(void)
{
    int peer, old_handle;
    MgRfb *r = test_display(&peer, 2, 2);
    uint8_t packet[32], before[512], after[512];
    size_t n = rect(packet, 0, 0, 2, 2, 0);
    char directory[] = "/tmp/megas-rfb-atomic-XXXXXX", path[128], error[256];
    struct stat old_stat, new_stat, preserved;
    struct rlimit saved, limited;
    void (*old_signal)(int);
    ssize_t old_size;
    DIR *files;
    struct dirent *entry;
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/Türkçe.png", directory);
    memset(packet + n, 10, 16);
    inject(r, peer, packet, n + 16, 2);
    assert(mg_rfb_png(r, path, "version=old", error, sizeof(error)) == 0);
    assert(stat(path, &old_stat) == 0 && (old_stat.st_mode & 0777) == 0600);
    old_handle = open(path, O_RDONLY); assert(old_handle >= 0);
    old_size = read(old_handle, before, sizeof(before)); assert(old_size > 0);
    r->pixels[0] = 222;
    assert(mg_rfb_png(r, path, "version=new", error, sizeof(error)) == 0);
    assert(stat(path, &new_stat) == 0 && new_stat.st_ino != old_stat.st_ino);
    assert(lseek(old_handle, 0, SEEK_SET) == 0);
    assert(read(old_handle, after, sizeof(after)) == old_size);
    assert(!memcmp(before, after, (size_t)old_size)); /* Existing reader stays stable. */
    assert(getrlimit(RLIMIT_FSIZE, &saved) == 0);
    limited = saved; limited.rlim_cur = 1;
    old_signal = signal(SIGXFSZ, SIG_IGN); assert(old_signal != SIG_ERR);
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    assert(mg_rfb_png(r, path, "must-not-replace", error, sizeof(error)) < 0);
    assert(setrlimit(RLIMIT_FSIZE, &saved) == 0);
    assert(signal(SIGXFSZ, old_signal) != SIG_ERR);
    assert(stat(path, &preserved) == 0);
    assert(preserved.st_ino == new_stat.st_ino && preserved.st_size == new_stat.st_size);
    files = opendir(directory); assert(files);
    while ((entry = readdir(files))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            assert(!strcmp(entry->d_name, "Türkçe.png")); /* No failed .part garbage. */
        }
    }
    closedir(files); close(old_handle); remove(path); rmdir(directory);
    close(peer); mg_rfb_free(r);
}

static void resizing(void)
{
    int peer;
    MgRfb *r = test_display(&peer, 4, 3);
    uint8_t packet[256], reply[256];
    char error[256];
    size_t n;
    assert(mg_rfb_resize(r, 8, 6) < 0); /* Capability is not yet observed. */
    n = rect(packet, 0, 0, 4, 3, -308);
    memset(packet + n, 0, 20); packet[n] = 1;
    mg_put16(packet + n + 12, 4); mg_put16(packet + n + 14, 3);
    inject(r, peer, packet, n + 20, 2);
    assert(r->ext_size && r->width == 4 && r->height == 3);
    assert(mg_rfb_resize(r, 8, 6) == 0);
    assert(recv(peer, reply, sizeof(reply), 0) == 34);
    assert(reply[10] == 251 && mg_get16(reply + 12) == 8);
    assert(mg_get16(reply + 26) == 8 && mg_get16(reply + 28) == 6);
    /* QEMU's 'forwarded' acknowledgement is not a completed resize. */
    n = rect(packet, 1, 4, 4, 3, -308);
    memset(packet + n, 0, 20); packet[n] = 1;
    mg_put16(packet + n + 12, 4); mg_put16(packet + n + 14, 3);
    inject(r, peer, packet, n + 20, 1);
    assert(r->width == 4 && r->height == 3);
    /* A rejection preserves the connection and the current framebuffer. */
    n = rect(packet, 1, 3, 4, 3, -308);
    assert(send(peer, packet, n + 20, MSG_NOSIGNAL) == (ssize_t)(n + 20));
    assert(mg_rfb_tick(r, error, sizeof(error)) == -2);
    assert(strstr(error, "rejected") && r->socket != MG_INVALID);
    assert(r->width == 4 && r->height == 3);
    n = rect(packet, 0, 0, 8, 6, -308);
    memset(packet + n, 0, 20); packet[n] = 1;
    mg_put16(packet + n + 12, 8); mg_put16(packet + n + 14, 6);
    inject(r, peer, packet, n + 20, 3);
    assert(r->width == 8 && r->height == 6);
    assert(!mg_rfb_pixels(r, NULL, NULL, NULL)); /* Await new guest pixels. */
    n = rect(packet, 0, 0, 2, 2, -223);
    inject(r, peer, packet, n, 1);
    assert(r->width == 2 && r->height == 2);
    assert(mg_rfb_resize(r, 4096, 4096) < 0);
    close(peer); mg_rfb_free(r);
}
static void continuous_and_auxiliary(void)
{
    int peer;
    MgRfb *r = test_display(&peer, 4, 3);
    uint8_t packet[256] = { 150 }, reply[256];
    size_t n;
    inject(r, peer, packet, 1, 1);
    assert(recv(peer, reply, sizeof(reply), 0) == 10);
    assert(reply[0] == 150 && reply[1] == 1);
    n = rect(packet, 0, 0, 1, 1, 0);
    packet[n] = 90; packet[n + 1] = 91; packet[n + 2] = 92; packet[n + 3] = 0;
    inject(r, peer, packet, n + 4, 3);
    assert(recv(peer, reply, sizeof(reply), 0) < 0 && mg_again());
    /* Bell, clipboard, colour map and empty update may be coalesced. */
    n = 0; packet[n++] = 2; packet[n++] = 3;
    memset(packet + n, 0, 7); mg_put32(packet + n + 3, 3); n += 7;
    memcpy(packet + n, "abc", 3); n += 3;
    packet[n++] = 1; memset(packet + n, 0, 5); mg_put16(packet + n + 3, 1); n += 5;
    memset(packet + n, 0, 6); n += 6;
    packet[n++] = 0; packet[n++] = 0; packet[n++] = 0; packet[n++] = 0;
    inject(r, peer, packet, n, 1);
    assert(r->parse == MG_MESSAGE);
    close(peer); mg_rfb_free(r);
}
static void malformed_and_disconnect(void)
{
    unsigned scenario;
    for (scenario = 0; scenario < 7; scenario++) {
        int peer;
        MgRfb *r = test_display(&peer, 4, 3);
        uint8_t packet[128] = { 0 };
        char error[256];
        size_t n;
        switch (scenario) {
        case 0: n = rect(packet, 3, 2, 2, 1, 0); break;
        case 1: n = rect(packet, 0, 0, 4, 3, 42); break;
        case 2: n = rect(packet, 0, 0, 4, 3, 1);
                mg_put16(packet + n, 4); mg_put16(packet + n + 2, 0); n += 4; break;
        case 3: n = rect(packet, 0, 0, 4096, 4096, -223); break;
        case 4: packet[0] = 3; mg_put32(packet + 4, 0xffffffffu); n = 8; break;
        case 5: packet[0] = 99; n = 1; break;
        default: n = rect(packet, 0, 0, 4, 3, -308); packet[n++] = 0;
                 packet[n++] = 0; packet[n++] = 0; packet[n++] = 0; break;
        }
        assert(send(peer, packet, n, MSG_NOSIGNAL) == (ssize_t)n);
        assert(mg_rfb_tick(r, error, sizeof(error)) == -1);
        assert(error[0] && r->socket == MG_INVALID);
        assert(!mg_rfb_pixels(r, NULL, NULL, NULL));
        assert(mg_rfb_key(r, 'a', 1) < 0);
        close(peer); mg_rfb_free(r);
    }
    {
        int peer;
        MgRfb *r = test_display(&peer, 4, 3);
        uint8_t packet[16]; char error[256];
        size_t n = rect(packet, 0, 0, 4, 3, 0);
        inject(r, peer, packet, n, 1); /* Disconnected in the middle of raw. */
        close(peer);
        assert(mg_rfb_tick(r, error, sizeof(error)) == -1);
        assert(!mg_rfb_pixels(r, NULL, NULL, NULL));
        assert(mg_rfb_png(r, "/tmp/should-not-exist.png", "", error, sizeof(error)) < 0);
        mg_rfb_free(r);
    }
}
static void bounded_send_and_stall(void)
{
    int peer, small = 2048;
    MgRfb *r = test_display(&peer, 4, 3);
    uint8_t readback[16384], packet[16];
    char error[256];
    unsigned accepted = 0;
    size_t read_bytes = 0;
    int n;
    assert(setsockopt(r->socket, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    while (mg_rfb_key(r, 0xff0d, 1) == 0) {
        accepted++;
        assert(accepted < 100000); /* It cannot allocate an unbounded queue. */
    }
    assert(r->socket != MG_INVALID && r->out_end <= MG_RFB_OUTPUT);
    assert(mg_rfb_pending(r) > 0);
    assert(mg_rfb_flush(r, 0, error, sizeof(error)) == -2);
    assert(strstr(error, "Do not repeat"));
    do {
        n = (int)recv(peer, readback + read_bytes, sizeof(readback) - read_bytes, 0);
        if (n > 0) { read_bytes += (size_t)n; }
        assert(mg_flush(r) == 0);
    } while (n > 0 || r->out_end > r->out_begin);
    assert(read_bytes == (size_t)accepted * 8u);
    assert(mg_rfb_flush(r, 1, error, sizeof(error)) == 0);
    assert(mg_rfb_pending(r) == 0);
    for (read_bytes = 0; read_bytes < (size_t)accepted * 8u; read_bytes += 8u) {
        assert(readback[read_bytes] == 4 && readback[read_bytes + 1] == 1);
        assert(mg_get32(readback + read_bytes + 4) == 0xff0d);
    }
    /* A completed unchanged desktop must never timeout merely for being idle. */
    r->last_progress = mg_milliseconds() - 3000u;
    assert(mg_rfb_tick(r, error, sizeof(error)) == 0);
    inject(r, peer, packet, rect(packet, 0, 0, 4, 3, 0), 1);
    r->last_progress = mg_milliseconds() - 3000u;
    assert(mg_rfb_tick(r, error, sizeof(error)) == -1);
    assert(strstr(error, "partway") && r->socket == MG_INVALID);
    close(peer); mg_rfb_free(r);
}

static void server_send(int fd, const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++) {
        assert(send(fd, data + i, 1, MSG_NOSIGNAL) == 1);
    }
}
static void server_read(int fd, uint8_t *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = recv(fd, data + done, size - done, 0);
        assert(n > 0); done += (size_t)n;
    }
}
static void handshake(unsigned password)
{
    int server = socket(AF_INET, SOCK_STREAM, 0), child_status;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    pid_t child;
    char error[256];
    MgRfb *r = mg_rfb_new();
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(server >= 0 && bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(server, (struct sockaddr *)&address, &length) == 0);
    assert(listen(server, 1) == 0);
    child = fork(); assert(child >= 0);
    if (!child) {
        int fd = accept(server, NULL, NULL);
        uint8_t packet[256];
        assert(fd >= 0);
        server_send(fd, (const uint8_t *)"RFB 003.008\n", 12);
        server_read(fd, packet, 12);
        assert(!memcmp(packet, "RFB 003.008\n", 12));
        packet[0] = 1; packet[1] = password ? 2 : 1;
        server_send(fd, packet, 2);
        if (!password) {
            server_read(fd, packet, 1); assert(packet[0] == 1);
            memset(packet, 0, 4); server_send(fd, packet, 4);
            server_read(fd, packet, 1); assert(packet[0] == 1);
            memset(packet, 0, 24); mg_put16(packet, 4); mg_put16(packet + 2, 3);
            mg_put32(packet + 20, 4); server_send(fd, packet, 24);
            server_send(fd, (const uint8_t *)"QEMU", 4);
            server_read(fd, packet, 54);
            assert(packet[0] == 0 && packet[4] == 32 && packet[6] == 0);
            assert(packet[7] == 1 && packet[15] == 8 && packet[16] == 16);
            assert(packet[20] == 2 && mg_get16(packet + 22) == 5);
            assert(packet[44] == 3 && packet[45] == 0);
        }
        close(fd); close(server); _exit(0);
    }
    if (password) {
        assert(mg_rfb_connect(r, "127.0.0.1", ntohs(address.sin_port), error, sizeof(error)) < 0);
        assert(strstr(error, "password"));
    } else {
        assert(mg_rfb_connect(r, "127.0.0.1", ntohs(address.sin_port), error, sizeof(error)) == 0);
        assert(!mg_rfb_pixels(r, NULL, NULL, NULL));
    }
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    close(server); mg_rfb_free(r);
}
int main(void)
{
    raw_and_input(); png_atomic_replacement(); resizing(); continuous_and_auxiliary();
    malformed_and_disconnect(); bounded_send_and_stall(); handshake(0); handshake(1);
    puts("RFB: fragmented handshake/raw, input, overlap, resize, continuous, bounds, disconnect and PNG passed");
    return 0;
}
#else
int main(void)
{
    assert(mg_dimensions(1920, 1080));
    assert(!mg_dimensions(4096, 4096));
    puts("RFB: Windows compile and framebuffer bound check passed; POSIX socket fixtures were skipped, and this is not a Windows 11 desktop run");
    return 0;
}
#endif
