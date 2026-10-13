/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int listener;
    unsigned port;
    int windows, bad_sync;
    unsigned open_count, write_count, close_count, exec_count, status_count;
    unsigned char uploaded[MG_GUEST_PAYLOAD_CAP];
    size_t uploaded_size;
    char error[256];
} FakeQga;

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64decode(const char *input, unsigned char *output, size_t capacity,
                     size_t *length)
{
    size_t i, used = 0, n = strlen(input);
    if (n % 4) return -1;
    for (i = 0; i < n; i += 4) {
        const char *p[4];
        int v[4], j;
        size_t count = input[i + 2] == '=' ? 1 : input[i + 3] == '=' ? 2 : 3;
        for (j = 0; j < 4; j++) {
            p[j] = strchr(b64, input[i + j]);
            v[j] = p[j] ? (int)(p[j] - b64) : 0;
        }
        if (used + count > capacity) return -1;
        output[used++] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
        if (count > 1) output[used++] = (unsigned char)((v[1] << 4) | (v[2] >> 2));
        if (count > 2) output[used++] = (unsigned char)((v[2] << 6) | v[3]);
    }
    *length = used;
    return 0;
}

static int b64encode(const unsigned char *input, size_t length,
                     char *output, size_t capacity)
{
    size_t i, used = 0;
    for (i = 0; i < length; i += 3) {
        unsigned a = input[i], c = i + 1 < length ? input[i + 1] : 0;
        unsigned d = i + 2 < length ? input[i + 2] : 0;
        if (used + 4 >= capacity) return -1;
        output[used++] = b64[a >> 2];
        output[used++] = b64[((a & 3) << 4) | (c >> 4)];
        output[used++] = i + 1 < length ? b64[((c & 15) << 2) | (d >> 6)] : '=';
        output[used++] = i + 2 < length ? b64[d & 63] : '=';
    }
    output[used] = 0;
    return 0;
}

static int read_line(int fd, char *out, size_t capacity, int *sync)
{
    size_t used = 0;
    *sync = 0;
    while (used + 1 < capacity) {
        unsigned char c;
        int n = (int)recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == 0xff) { *sync = 1; continue; }
        if (c == '\n') { out[used] = 0; return (int)used; }
        out[used++] = (char)c;
    }
    return -1;
}

static uint64_t outer_id(const char *line)
{
    const char *p = line, *last = NULL;
    uint64_t value = 0;
    while ((p = strstr(p, "\"id\":")) != NULL) { last = p; p += 5; }
    if (!last) return 0;
    sscanf(last + 5, "%llu", (unsigned long long *)&value);
    return value;
}

static uint64_t sync_id(const char *line)
{
    const char *p = strstr(line, "\"arguments\":{\"id\":");
    unsigned long long value = 0;
    if (!p) return 0;
    p += strlen("\"arguments\":{\"id\":");
    sscanf(p, "%llu", &value);
    return (uint64_t)value;
}

static int send_fragmented(int fd, const char *text, int sentinel)
{
    struct timespec pause = {0, 300000};
    size_t i, n = strlen(text);
    if (sentinel) {
        unsigned char marker = 0xff;
        if (send(fd, &marker, 1, 0) != 1) return -1;
    }
    for (i = 0; i < n; i++) {
        if (send(fd, text + i, 1, 0) != 1) return -1;
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int respond(FakeQga *fake, int fd, const char *request,
                   const char *execute, int sync)
{
    char response[8192];
    uint64_t id = outer_id(request);
    if (!id) { snprintf(fake->error, sizeof(fake->error), "request id missing: %.100s", request); return -1; }
    if (!strcmp(execute, "guest-sync-delimited")) {
        const char stale[] = "stale-line\n";
        char payload[128];
        if (send(fd, stale, sizeof(stale) - 1, 0) != (ssize_t)(sizeof(stale) - 1)) return -1;
        snprintf(payload, sizeof(payload), "{\"return\":%llu,\"id\":%llu}\n",
                 (unsigned long long)(sync_id(request) + (fake->bad_sync ? 1 : 0)),
                 (unsigned long long)id);
        return send_fragmented(fd, payload, 1);
    }
    (void)sync;
    if (!strcmp(execute, "guest-get-osinfo")) {
        const char *name = fake->windows ? "Microsoft Windows 11" : "Ubuntu 24.04 LTS";
        const char *osid = fake->windows ? "windows" : "ubuntu";
        snprintf(response, sizeof(response),
                 "{\"return\":{\"id\":\"%s\",\"name\":\"%s\",\"pretty-name\":\"%s\",\"kernel-release\":\"%s\"},\"id\":%llu}\n",
                 osid, name, name, fake->windows ? "10.0.26100" : "6.8.0-31-generic",
                 (unsigned long long)id);
    } else if (!strcmp(execute, "guest-file-open")) {
        fake->open_count++;
        assert(strstr(request, "megas-linux-policy-"));
        snprintf(response, sizeof(response), "{\"return\":42,\"id\":%llu}\n", (unsigned long long)id);
    } else if (!strcmp(execute, "guest-file-write")) {
        const char *start = strstr(request, "\"buf-b64\":\"");
        char encoded[24000];
        size_t bytes = 0;
        const char *end;
        fake->write_count++;
        assert(start);
        start += strlen("\"buf-b64\":\"");
        end = strchr(start, '"');
        assert(end && (size_t)(end - start) < sizeof(encoded));
        memcpy(encoded, start, (size_t)(end - start)); encoded[end - start] = 0;
        assert(!b64decode(encoded, fake->uploaded + fake->uploaded_size,
                          sizeof(fake->uploaded) - fake->uploaded_size, &bytes));
        fake->uploaded_size += bytes;
        snprintf(response, sizeof(response),
                 "{\"return\":{\"count\":%llu,\"eof\":false},\"id\":%llu}\n",
                 (unsigned long long)bytes, (unsigned long long)id);
    } else if (!strcmp(execute, "guest-file-close")) {
        fake->close_count++;
        snprintf(response, sizeof(response), "{\"return\":{},\"id\":%llu}\n", (unsigned long long)id);
    } else if (!strcmp(execute, "guest-exec")) {
        const char *start = strstr(request, "\"input-data\":\"");
        char encoded[512];
        unsigned char decoded[256];
        const char *end;
        size_t decoded_size = 0;
        fake->exec_count++;
        assert(strstr(request, "\"path\":\"/bin/sh\""));
        assert(strstr(request, "\"--mode=password\""));
        assert(start && !strstr(request, "S3cret"));
        start += strlen("\"input-data\":\"");
        end = strchr(start, '\"');
        assert(end && (size_t)(end - start) < sizeof(encoded));
        memcpy(encoded, start, (size_t)(end - start)); encoded[end - start] = 0;
        assert(!b64decode(encoded, decoded, sizeof(decoded), &decoded_size));
        assert(decoded_size == strlen("S3cret") + 1);
        assert(!memcmp(decoded, "S3cret\n", decoded_size));
        snprintf(response, sizeof(response), "{\"return\":{\"pid\":777},\"id\":%llu}\n",
                 (unsigned long long)id);
    } else if (!strcmp(execute, "guest-exec-status")) {
        fake->status_count++;
        if (fake->status_count == 1) {
            snprintf(response, sizeof(response), "{\"return\":{\"exited\":false},\"id\":%llu}\n",
                     (unsigned long long)id);
        } else {
            const unsigned char output[] = "Guest account policy applied.\n";
            char encoded[128];
            assert(!b64encode(output, sizeof(output) - 1, encoded, sizeof(encoded)));
            snprintf(response, sizeof(response),
                     "{\"return\":{\"exited\":true,\"exitcode\":0,\"out-data\":\"%s\"},\"id\":%llu}\n",
                     encoded, (unsigned long long)id);
        }
    } else {
        snprintf(fake->error, sizeof(fake->error), "unexpected guest command: %.100s", execute);
        return -1;
    }
    return send_fragmented(fd, response, 0);
}

static void *fake_qga_thread(void *data)
{
    FakeQga *fake = data;
    int fd = accept(fake->listener, NULL, NULL);
    int sync = 0;
    char line[MG_GUEST_MESSAGE_CAP];
    unsigned total = 0;
    if (fd < 0) { snprintf(fake->error, sizeof(fake->error), "accept failed"); return NULL; }
    for (;;) {
        int n = read_line(fd, line, sizeof(line), &sync);
        char execute[64] = {0};
        if (n < 0) break;
        if (sscanf(line, "{\"execute\":\"%63[^\"]", execute) != 1) {
            snprintf(fake->error, sizeof(fake->error), "bad request: %.100s", line); break;
        }
        total++;
        if (respond(fake, fd, line, execute, sync)) break;
        if (fake->windows && !strcmp(execute, "guest-get-osinfo")) break;
        if (!strcmp(execute, "guest-exec-status") && fake->status_count == 2) break;
    }
    close(fd);
    if (!fake->windows && !fake->bad_sync && total < 8) snprintf(fake->error, sizeof(fake->error), "only %u QGA requests completed", total);
    return NULL;
}

static unsigned start_fake(FakeQga *fake, int windows, pthread_t *thread)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    memset(fake, 0, sizeof(*fake));
    fake->windows = windows;
    fake->listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(fake->listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(!bind(fake->listener, (struct sockaddr *)&address, sizeof(address)));
    assert(!getsockname(fake->listener, (struct sockaddr *)&address, &length));
    assert(!listen(fake->listener, 1));
    fake->port = ntohs(address.sin_port);
    assert(!pthread_create(thread, NULL, fake_qga_thread, fake));
    return fake->port;
}

static void read_helper(unsigned char *out, size_t capacity, size_t *length)
{
    FILE *file = fopen("guest/linux-policy.sh", "rb");
    assert(file);
    *length = fread(out, 1, capacity, file);
    assert(feof(file) && !ferror(file));
    fclose(file);
}

static void test_fragmented_policy(void)
{
    FakeQga fake;
    pthread_t thread;
    MgGuestJob job;
    char error[2048];
    uint64_t end;
    int result = 0;
    unsigned char expected[MG_GUEST_PAYLOAD_CAP];
    size_t expected_size = 0;
    unsigned port = start_fake(&fake, 0, &thread);
    memset(&job, 0, sizeof(job)); job.socket = MGP_BAD_SOCKET;
    assert(!mg_guest_policy_start(&job, port, "linux", 0, "megas", "S3cret", "guest",
                                  error, sizeof(error)));
    end = mgp_now() + 15000;
    while (mgp_now() < end && result == 0) {
        result = mg_guest_poll(&job, error, sizeof(error));
        mgp_sleep(2);
    }
    assert(result == 1);
    assert(!strstr(error, "S3cret"));
    assert(strstr(job.progress, "Guest account policy applied."));
    assert(job.pid == 777);
    assert(!job.password[0]);
    read_helper(expected, sizeof(expected), &expected_size);
    assert(fake.uploaded_size == expected_size);
    assert(!memcmp(fake.uploaded, expected, expected_size));
    assert(fake.open_count == 1 && fake.write_count == 1 && fake.close_count == 1);
    assert(fake.exec_count == 1 && fake.status_count == 2);
    assert(!fake.error[0]);
    mg_guest_clear(&job);
    close(fake.listener);
    pthread_join(thread, NULL);
}

static void test_windows_doom_rejected(void)
{
    FakeQga fake;
    pthread_t thread;
    MgGuestJob job;
    char error[2048];
    int result = 0;
    uint64_t end;
    unsigned port = start_fake(&fake, 1, &thread);
    memset(&job, 0, sizeof(job)); job.socket = MGP_BAD_SOCKET;
    assert(!mg_guest_doom_start(&job, port, "guest", NULL, error, sizeof(error)));
    end = mgp_now() + 5000;
    while (mgp_now() < end && result == 0) {
        result = mg_guest_poll(&job, error, sizeof(error));
        mgp_sleep(2);
    }
    assert(result == -1);
    assert(strstr(error, "confirmed Linux guest"));
    assert(fake.open_count == 0 && fake.write_count == 0 && fake.exec_count == 0);
    assert(!fake.error[0]);
    mg_guest_clear(&job);
    close(fake.listener);
    pthread_join(thread, NULL);
}

static void test_missing_qga_repair_prompt(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    MgGuestJob job;
    char error[1024];
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    unsigned port;
    assert(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(listener, (struct sockaddr *)&address, sizeof(address)));
    assert(!getsockname(listener, (struct sockaddr *)&address, &length));
    port = ntohs(address.sin_port);
    close(listener);
    memset(&job, 0, sizeof(job)); job.socket = MGP_BAD_SOCKET;
    assert(mg_guest_policy_start(&job, port, "linux", 1, "megas", NULL, "guest",
                                error, sizeof(error)) == -1);
    assert(strstr(error, "QEMU Guest Agent"));
    assert(strstr(error, "No guest account change was attempted"));
    assert(!job.active);
    mg_guest_clear(&job);
}

static void test_sync_nonce_mismatch_rejected(void)
{
    FakeQga fake;
    pthread_t thread;
    MgGuestJob job;
    char error[2048];
    uint64_t end;
    int result = 0;
    unsigned port = start_fake(&fake, 0, &thread);
    fake.bad_sync = 1;
    memset(&job, 0, sizeof(job)); job.socket = MGP_BAD_SOCKET;
    assert(!mg_guest_policy_start(&job, port, "linux", 1, "megas", NULL, "guest",
                                  error, sizeof(error)));
    end = mgp_now() + 5000;
    while (mgp_now() < end && result == 0) {
        result = mg_guest_poll(&job, error, sizeof(error));
        mgp_sleep(2);
    }
    assert(result == -1);
    assert(strstr(error, "synchronization nonce"));
    assert(fake.open_count == 0 && fake.write_count == 0 && fake.exec_count == 0);
    assert(!fake.error[0]);
    mg_guest_clear(&job);
    close(fake.listener);
    pthread_join(thread, NULL);
}

int main(void)
{
    test_fragmented_policy();
    test_windows_doom_rejected();
    test_missing_qga_repair_prompt();
    test_sync_nonce_mismatch_rejected();
    puts("guest bridge: fragmented QGA transfer, identity refusal, sync nonce, and missing-agent prompt verified");
    return 0;
}
