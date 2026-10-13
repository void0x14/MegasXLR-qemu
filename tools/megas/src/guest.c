/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The controller deliberately speaks a small, bounded subset of QGA's JSON
 * protocol. A request is never larger than 64 KiB and only one request is in
 * flight on this persistent loopback connection. */
#define QGA_TIMEOUT_MS 20000u
#define QGA_CONNECT_MS 120u
#define QGA_POLL_MS 700u
#define QGA_WRITE_CHUNK 16384u
#define QGA_MAX_TOKENS 2048

enum {
    JOB_POLICY = 1,
    JOB_DOOM = 2
};
enum {
    ST_IDLE = 0,
    ST_SYNC,
    ST_OSINFO,
    ST_FILE_OPEN,
    ST_FILE_WRITE,
    ST_FILE_CLOSE,
    ST_EXEC,
    ST_EXEC_STATUS,
    ST_DONE,
    ST_FAILED
};
enum { OS_ANY = 0, OS_WINDOWS = 1, OS_LINUX = 2 };

typedef struct {
    size_t start, end;
    int parent, type;
} JToken;
typedef struct {
    const char *s;
    size_t length, pos;
    int count, depth;
    JToken tokens[QGA_MAX_TOKENS];
} JParser;
enum { JT_OBJECT = '{', JT_ARRAY = '[', JT_STRING = 's', JT_PRIMITIVE = 'p' };

static void wipe(void *data, size_t length)
{
    volatile unsigned char *p = data;
    while (length--) *p++ = 0;
}

static void close_socket(MgGuestJob *job)
{
    if (job->socket != MGP_BAD_SOCKET) {
        mgp_close(job->socket);
        job->socket = MGP_BAD_SOCKET;
    }
}

static void wipe_secret_buffers(MgGuestJob *job)
{
    wipe(job->password, sizeof(job->password));
    wipe(job->tx, sizeof(job->tx));
    wipe(job->payload, sizeof(job->payload));
    job->password_pending = 0;
    job->payload_used = job->payload_sent = 0;
    job->tx_used = job->tx_sent = 0;
}

static void fail_job(MgGuestJob *job, char *error, size_t capacity,
                     const char *detail, int uncertain)
{
    if (job->kind && job->stage >= ST_FILE_OPEN && job->stage <= ST_EXEC_STATUS) {
        job->uncertain = uncertain || job->pid != 0 || job->file_open;
    }
    job->active = 0;
    job->stage = ST_FAILED;
    close_socket(job);
    snprintf(job->progress, sizeof(job->progress), "Guest operation failed; no success was reported.");
    if (capacity) {
        snprintf(error, capacity,
                 "Copy this to your agent: Repair the Megas guest operation. %s%s. "
                 "Confirm the QEMU Guest Agent and intended guest identity, then inspect the guest policy/Doom journal before retrying.%s",
                 detail, job->uncertain ? " The guest may have partially applied the requested change" : "",
                 job->kind == JOB_DOOM ? " Preserve the host and use only a guest-owned WAD." : " Preserve the host and existing guest accounts.");
    }
    wipe_secret_buffers(job);
}

static void finish_job(MgGuestJob *job)
{
    job->active = 0;
    job->stage = ST_DONE;
    job->uncertain = 0;
    close_socket(job);
    wipe_secret_buffers(job);
}

static int valid_utf8_text(const char *s, size_t max, int allow_empty)
{
    size_t i = 0, n;
    if (!s) return allow_empty;
    n = strlen(s);
    if ((!allow_empty && !n) || n > max) return 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i++];
        unsigned cp, minimum;
        size_t more, j;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return 0;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1f; more = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0f; more = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 0x07; more = 3; minimum = 0x10000; }
        else return 0;
        if (i + more > n) return 0;
        for (j = 0; j < more; j++) {
            unsigned char tail = (unsigned char)s[i++];
            if ((tail & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (tail & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        if (cp >= 0x80 && cp <= 0x9f) return 0;
    }
    return 1;
}

static int valid_username(const char *username)
{
    size_t i, n;
    char lower[33];
    if (!username || !(n = strlen(username)) || n > 32) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)username[i];
        int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        int digit = c >= '0' && c <= '9';
        if (!(letter || digit || c == '_' || c == '-')) return 0;
        if (i == 0 && !(letter || c == '_')) return 0;
        lower[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    lower[n] = 0;
    return strcmp(lower, "root") && strcmp(lower, "administrator") &&
           strcmp(lower, "guest");
}

static int expected_os(const char *os)
{
    char lower[96];
    size_t i, n;
    if (!os || !*os) return OS_ANY;
    n = strlen(os);
    if (n >= sizeof(lower)) return -1;
    for (i = 0; i <= n; i++) lower[i] = (char)tolower((unsigned char)os[i]);
    if (strstr(lower, "windows") || !strcmp(lower, "win")) return OS_WINDOWS;
    if (strstr(lower, "linux") || strstr(lower, "ubuntu") ||
        strstr(lower, "debian") || strstr(lower, "fedora") ||
        strstr(lower, "red hat") || strstr(lower, "rhel") ||
        strstr(lower, "centos") || strstr(lower, "rocky") ||
        strstr(lower, "alpine") || strstr(lower, "arch") ||
        strstr(lower, "opensuse") || strstr(lower, "mint")) return OS_LINUX;
    return -1;
}

static int json_quote(char *out, size_t capacity, size_t *used,
                      const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    if (*used + 2 >= capacity) return -1;
    out[(*used)++] = '"';
    while (*p) {
        char escape[7];
        const char *put = NULL;
        switch (*p) {
        case '"': put = "\\\""; break;
        case '\\': put = "\\\\"; break;
        case '\b': put = "\\b"; break;
        case '\f': put = "\\f"; break;
        case '\n': put = "\\n"; break;
        case '\r': put = "\\r"; break;
        case '\t': put = "\\t"; break;
        default:
            if (*p < 0x20) {
                snprintf(escape, sizeof(escape), "\\u%04x", *p);
                put = escape;
            }
            break;
        }
        if (put) {
            size_t n = strlen(put);
            if (*used + n >= capacity) return -1;
            memcpy(out + *used, put, n);
            *used += n;
        } else {
            if (*used + 1 >= capacity) return -1;
            out[(*used)++] = (char)*p;
        }
        p++;
    }
    if (*used + 1 >= capacity) return -1;
    out[(*used)++] = '"';
    out[*used] = 0;
    return 0;
}

static int request_begin(MgGuestJob *job, unsigned stage, const char *command,
                         const char *arguments, int sync)
{
    int n;
    uint64_t now = mgp_now();
    if (job->awaiting || job->tx_sent != job->tx_used) return -1;
    job->tx_used = job->tx_sent = 0;
    if (sync) job->tx[job->tx_used++] = (char)0xff;
    job->request_id++;
    n = snprintf(job->tx + job->tx_used, sizeof(job->tx) - job->tx_used,
                 "{\"execute\":\"%s\",\"arguments\":%s,\"id\":%llu}\n",
                 command, arguments ? arguments : "{}",
                 (unsigned long long)job->request_id);
    if (n < 0 || (size_t)n >= sizeof(job->tx) - job->tx_used) return -1;
    job->tx_used += (size_t)n;
    if (job->tx_used > MG_GUEST_MESSAGE_CAP) return -1;
    job->stage = stage;
    job->awaiting = 1;
    job->deadline = now + QGA_TIMEOUT_MS;
    job->rx_used = 0;
    if (stage == ST_SYNC) job->sync_sentinel = 0;
    if (stage == ST_FILE_OPEN || stage == ST_FILE_WRITE ||
        stage == ST_FILE_CLOSE || stage == ST_EXEC || stage == ST_EXEC_STATUS) {
        /* After a mutation is sent, a lost response must never trigger an
         * automatic retry. We still report the helper's observed outcome. */
        job->uncertain = 1;
    }
    return 0;
}

static int start_connection(MgGuestJob *job, char *error, size_t capacity)
{
    char args[96];
    uint64_t nonce = 0;
    if (mgp_init() || mgp_random(&nonce, sizeof(nonce))) {
        if (capacity) snprintf(error, capacity,
            "Copy this to your agent: Repair the Megas guest connection. Secure random initialization failed; no guest command ran.");
        return -1;
    }
    /* QGA's schema uses a signed 64-bit integer for guest-sync-delimited. */
    nonce &= INT64_MAX;
    if (!nonce) nonce = 1;
    job->request_id = (nonce & 0x3fffffffU) + 1;
    job->file_handle = nonce;
    job->sync_nonce = nonce;
    if (job->port < 1 || job->port > 65535) {
        if (capacity) snprintf(error, capacity,
            "Copy this to your agent: Repair the Megas guest connection. QEMU Guest Agent loopback port is invalid; no guest command ran.");
        return -1;
    }
    job->socket = mgp_connect(job->port, QGA_CONNECT_MS);
    if (job->socket == MGP_BAD_SOCKET) {
        if (capacity) snprintf(error, capacity,
            "Copy this to your agent: Start the intended VM with QEMU Guest Agent enabled and its virtio-serial channel exposed on loopback port %u. No guest account change was attempted.",
            job->port);
        return -1;
    }
    if (snprintf(args, sizeof(args), "{\"id\":%llu}",
                 (unsigned long long)nonce) >= (int)sizeof(args) ||
        request_begin(job, ST_SYNC, "guest-sync-delimited", args, 1)) {
        close_socket(job);
        if (capacity) snprintf(error, capacity,
            "Copy this to your agent: Repair the Megas QEMU Guest Agent connection. The bounded sync request could not be queued; no guest change ran.");
        return -1;
    }
    job->active = 1;
    snprintf(job->progress, sizeof(job->progress),
             "Connected to QEMU Guest Agent; confirming guest identity before changes.");
    return 0;
}

static void skip_space(JParser *p)
{
    while (p->pos < p->length && isspace((unsigned char)p->s[p->pos])) p->pos++;
}

static int parse_value(JParser *p, int parent);

static int parse_string(JParser *p, int parent)
{
    int token;
    if (p->pos >= p->length || p->s[p->pos] != '"' ||
        p->count >= QGA_MAX_TOKENS) return -1;
    token = p->count++;
    p->tokens[token].parent = parent;
    p->tokens[token].type = JT_STRING;
    p->tokens[token].start = p->pos++;
    while (p->pos < p->length) {
        unsigned char c = (unsigned char)p->s[p->pos++];
        if (c == '"') {
            p->tokens[token].end = p->pos;
            return token;
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            size_t i;
            if (p->pos >= p->length) return -1;
            c = (unsigned char)p->s[p->pos++];
            if (c == 'u') {
                for (i = 0; i < 4; i++) {
                    if (p->pos >= p->length ||
                        !isxdigit((unsigned char)p->s[p->pos++])) return -1;
                }
            } else if (!strchr("\"\\/bfnrt", c)) return -1;
        }
    }
    return -1;
}

static int parse_value(JParser *p, int parent)
{
    int token, open, close;
    if (p->count >= QGA_MAX_TOKENS || ++p->depth > 32) return -1;
    skip_space(p);
    if (p->pos >= p->length) { p->depth--; return -1; }
    if (p->s[p->pos] == '"') {
        token = parse_string(p, parent);
        p->depth--;
        return token;
    }
    token = p->count++;
    p->tokens[token].parent = parent;
    p->tokens[token].start = p->pos;
    if (p->s[p->pos] == '{' || p->s[p->pos] == '[') {
        open = p->s[p->pos++];
        close = open == '{' ? '}' : ']';
        p->tokens[token].type = open;
        skip_space(p);
        while (p->pos < p->length && p->s[p->pos] != close) {
            if (open == '{') {
                if (parse_string(p, token) < 0) goto bad;
                skip_space(p);
                if (p->pos >= p->length || p->s[p->pos++] != ':') goto bad;
            }
            if (parse_value(p, token) < 0) goto bad;
            skip_space(p);
            if (p->pos < p->length && p->s[p->pos] == close) break;
            if (p->pos >= p->length || p->s[p->pos++] != ',') goto bad;
            skip_space(p);
        }
        if (p->pos >= p->length || p->s[p->pos++] != close) goto bad;
    } else {
        p->tokens[token].type = JT_PRIMITIVE;
        while (p->pos < p->length &&
               !isspace((unsigned char)p->s[p->pos]) &&
               p->s[p->pos] != ',' && p->s[p->pos] != ']' &&
               p->s[p->pos] != '}') p->pos++;
        if (p->pos == p->tokens[token].start) goto bad;
        if (!((p->s[p->tokens[token].start] == 't' &&
               p->pos - p->tokens[token].start == 4 &&
               !memcmp(p->s + p->tokens[token].start, "true", 4)) ||
              (p->s[p->tokens[token].start] == 'f' &&
               p->pos - p->tokens[token].start == 5 &&
               !memcmp(p->s + p->tokens[token].start, "false", 5)) ||
              (p->s[p->tokens[token].start] == 'n' &&
               p->pos - p->tokens[token].start == 4 &&
               !memcmp(p->s + p->tokens[token].start, "null", 4)) ||
              isdigit((unsigned char)p->s[p->tokens[token].start]) ||
              p->s[p->tokens[token].start] == '-')) goto bad;
    }
    p->tokens[token].end = p->pos;
    p->depth--;
    return token;
bad:
    p->depth--;
    return -1;
}

static int json_parse(JParser *p, const char *s, size_t length)
{
    memset(p, 0, sizeof(*p));
    p->s = s;
    p->length = length;
    if (parse_value(p, -1) < 0) return -1;
    skip_space(p);
    return p->pos == p->length ? 0 : -1;
}

static int json_eq(JParser *p, int token, const char *text)
{
    size_t start, end;
    if (token < 0 || p->tokens[token].type != JT_STRING) return 0;
    start = p->tokens[token].start + 1;
    end = p->tokens[token].end - 1;
    return end - start == strlen(text) && !memcmp(p->s + start, text, end - start);
}

static int json_get(JParser *p, int parent, const char *key)
{
    int i;
    if (parent < 0 || p->tokens[parent].type != JT_OBJECT) return -1;
    for (i = parent + 1; i + 1 < p->count;) {
        int value;
        if (p->tokens[i].parent != parent || p->tokens[i].type != JT_STRING) {
            i++;
            continue;
        }
        value = i + 1;
        if (json_eq(p, i, key)) return value;
        i = value + 1;
        while (i < p->count && p->tokens[i].start < p->tokens[value].end) i++;
    }
    return -1;
}

static int append_utf8(char *out, size_t capacity, size_t *used, unsigned cp)
{
    unsigned char bytes[4];
    size_t count, i;
    if (cp >= 0xd800 && cp <= 0xdfff) return -1;
    if (cp < 0x80) { bytes[0] = (unsigned char)cp; count = 1; }
    else if (cp < 0x800) {
        bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 63)); count = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 63));
        bytes[2] = (unsigned char)(0x80 | (cp & 63)); count = 3;
    } else {
        bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 63));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 63));
        bytes[3] = (unsigned char)(0x80 | (cp & 63)); count = 4;
    }
    if (*used + count >= capacity) return -1;
    for (i = 0; i < count; i++) out[(*used)++] = (char)bytes[i];
    return 0;
}

static int hex4(const char *s)
{
    int i, value = 0;
    for (i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s[i];
        value = value * 16 + (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    return value;
}

static int json_string(JParser *p, int token, char *out, size_t capacity)
{
    size_t i, end, used = 0;
    if (token < 0 || !capacity || p->tokens[token].type != JT_STRING) return -1;
    i = p->tokens[token].start + 1;
    end = p->tokens[token].end - 1;
    while (i < end) {
        unsigned cp = (unsigned char)p->s[i++];
        if (cp == '\\') {
            cp = (unsigned char)p->s[i++];
            switch (cp) {
            case '"': case '\\': case '/': break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u':
                cp = (unsigned)hex4(p->s + i); i += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    unsigned low;
                    if (i + 6 > end || p->s[i] != '\\' || p->s[i + 1] != 'u') return -1;
                    low = (unsigned)hex4(p->s + i + 2);
                    if (low < 0xdc00 || low > 0xdfff) return -1;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    i += 6;
                }
                break;
            default: return -1;
            }
        }
        if (append_utf8(out, capacity, &used, cp)) return -1;
    }
    out[used] = 0;
    return 0;
}

static int json_uint(JParser *p, int token, uint64_t *value)
{
    size_t i, end;
    uint64_t n = 0;
    if (token < 0 || p->tokens[token].type != JT_PRIMITIVE) return -1;
    i = p->tokens[token].start;
    end = p->tokens[token].end;
    if (i == end) return -1;
    for (; i < end; i++) {
        unsigned digit;
        if (!isdigit((unsigned char)p->s[i])) return -1;
        digit = (unsigned)(p->s[i] - '0');
        if (n > (UINT64_MAX - digit) / 10) return -1;
        n = n * 10 + digit;
    }
    *value = n;
    return 0;
}

static int json_true(JParser *p, int token)
{
    return token >= 0 && p->tokens[token].type == JT_PRIMITIVE &&
           p->tokens[token].end - p->tokens[token].start == 4 &&
           !memcmp(p->s + p->tokens[token].start, "true", 4);
}

static int b64_encode(const unsigned char *input, size_t length,
                      char *output, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, used = 0;
    for (i = 0; i < length; i += 3) {
        unsigned a = input[i], b = i + 1 < length ? input[i + 1] : 0;
        unsigned c = i + 2 < length ? input[i + 2] : 0;
        if (used + 4 >= capacity) return -1;
        output[used++] = alphabet[a >> 2];
        output[used++] = alphabet[((a & 3) << 4) | (b >> 4)];
        output[used++] = i + 1 < length ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        output[used++] = i + 2 < length ? alphabet[c & 63] : '=';
    }
    output[used] = 0;
    return (int)used;
}

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *input, size_t length,
                      unsigned char *output, size_t capacity, size_t *written)
{
    size_t i, used = 0;
    if (length % 4) return -1;
    for (i = 0; i < length; i += 4) {
        int a = b64_value((unsigned char)input[i]);
        int b = b64_value((unsigned char)input[i + 1]);
        int c = input[i + 2] == '=' ? 0 : b64_value((unsigned char)input[i + 2]);
        int d = input[i + 3] == '=' ? 0 : b64_value((unsigned char)input[i + 3]);
        size_t count = input[i + 2] == '=' ? 1 : input[i + 3] == '=' ? 2 : 3;
        if (a < 0 || b < 0 || c < 0 || d < 0 || used + count >= capacity) return -1;
        output[used++] = (unsigned char)((a << 2) | (b >> 4));
        if (count > 1) output[used++] = (unsigned char)((b << 4) | (c >> 2));
        if (count > 2) output[used++] = (unsigned char)((c << 6) | d);
        if (count < 3 && i + 4 != length) return -1;
    }
    output[used] = 0;
    *written = used;
    return 0;
}

static int json_arg(char *out, size_t capacity, size_t *used, const char *arg)
{
    if (*used + 1 >= capacity) return -1;
    out[(*used)++] = ',';
    return json_quote(out, capacity, used, arg);
}

static int load_helper(MgGuestJob *job, const char *name)
{
    char path[1200];
    FILE *file;
    size_t n;
    int sep = job->helpers[0] && job->helpers[strlen(job->helpers) - 1] != '/' &&
              job->helpers[strlen(job->helpers) - 1] != '\\';
    if (snprintf(path, sizeof(path), "%s%s%s", job->helpers, sep ? "/" : "", name) >= (int)sizeof(path)) return -1;
    file = mgp_fopen(path, "rb");
    if (!file) return -1;
    n = fread(job->payload, 1, sizeof(job->payload), file);
    if (ferror(file) || (!feof(file) && n == sizeof(job->payload))) {
        fclose(file); return -1;
    }
    if (fclose(file) || !n || memchr(job->payload, 0, n)) return -1;
    job->payload_used = n;
    job->payload_sent = 0;
    return 0;
}

static int queue_file_open(MgGuestJob *job)
{
    char args[512];
    size_t used = 0;
    int n;
    n = snprintf(args, sizeof(args), "{\"path\":");
    if (n < 0 || (size_t)n >= sizeof(args)) return -1;
    used = (size_t)n;
    if (json_quote(args, sizeof(args), &used, job->guest_path)) return -1;
    n = snprintf(args + used, sizeof(args) - used, ",\"mode\":\"wb\"}");
    if (n < 0 || (size_t)n >= sizeof(args) - used) return -1;
    return request_begin(job, ST_FILE_OPEN, "guest-file-open", args, 0);
}

static int queue_file_write(MgGuestJob *job)
{
    char args[24000], encoded[QGA_WRITE_CHUNK * 4 / 3 + 8];
    size_t left = job->payload_used - job->payload_sent;
    size_t count = left > QGA_WRITE_CHUNK ? QGA_WRITE_CHUNK : left;
    int n, b64;
    if (!count) return -1;
    b64 = b64_encode(job->payload + job->payload_sent, count,
                     encoded, sizeof(encoded));
    if (b64 < 0) return -1;
    n = snprintf(args, sizeof(args), "{\"handle\":%llu,\"buf-b64\":\"%s\"}",
                 (unsigned long long)job->file_handle, encoded);
    wipe(encoded, sizeof(encoded));
    if (n < 0 || (size_t)n >= sizeof(args)) return -1;
    return request_begin(job, ST_FILE_WRITE, "guest-file-write", args, 0);
}

static int queue_file_close(MgGuestJob *job)
{
    char args[96];
    int n = snprintf(args, sizeof(args), "{\"handle\":%llu}",
                     (unsigned long long)job->file_handle);
    if (n < 0 || (size_t)n >= sizeof(args)) return -1;
    return request_begin(job, ST_FILE_CLOSE, "guest-file-close", args, 0);
}

static int queue_exec(MgGuestJob *job)
{
    char args[8192], argv[4096], encoded[512];
    size_t used = 0;
    int n;
    int secret_request = !job->enabled && job->password[0] && job->kind == JOB_POLICY;
    const char *program;
    args[0] = argv[0] = 0;
    if (job->windows) {
        program = "powershell.exe";
#define WARG(x) do { if (json_arg(argv, sizeof(argv), &used, (x))) return -1; } while (0)
        if (json_quote(argv, sizeof(argv), &used, "-NoProfile")) return -1;
        WARG("-NonInteractive"); WARG("-ExecutionPolicy"); WARG("Bypass");
        WARG("-File"); WARG(job->guest_path); WARG("-Apply"); WARG("-SelfDelete");
        WARG("-User"); WARG(job->username);
        WARG("-Mode");
        if (job->enabled) WARG("Passwordless");
        else if (job->password[0]) WARG("Password");
        else WARG("Restore");
        if (!job->enabled && job->password[0]) WARG("-ReadPasswordFromStdin");
#undef WARG
    } else {
        program = "/bin/sh";
#define LARG(x) do { if (json_arg(argv, sizeof(argv), &used, (x))) return -1; } while (0)
        if (json_quote(argv, sizeof(argv), &used, job->guest_path)) return -1;
        LARG("--apply"); LARG("--self-delete");
        {
            char user_arg[48];
            if (snprintf(user_arg, sizeof(user_arg), "--user=%s", job->username) >= (int)sizeof(user_arg)) return -1;
            LARG(user_arg);
        }
        LARG(job->enabled ? "--mode=passwordless" :
             (job->password[0] ? "--mode=password" : "--mode=restore"));
#undef LARG
    }
    if (job->kind == JOB_DOOM) {
        /* Doom's interpreter and arguments are separate from password policy. */
        program = "/bin/sh";
        used = 0; argv[0] = 0;
        if (json_quote(argv, sizeof(argv), &used, job->guest_path)) return -1;
#define DARG(x) do { if (json_arg(argv, sizeof(argv), &used, (x))) return -1; } while (0)
        DARG("--apply"); DARG("--self-delete");
        {
            char user_arg[48];
            if (snprintf(user_arg, sizeof(user_arg), "--user=%s", job->username) >= (int)sizeof(user_arg)) return -1;
            DARG(user_arg);
        }
        if (job->wad[0]) {
            char wad_arg[1032];
            if (snprintf(wad_arg, sizeof(wad_arg), "--wad=%s", job->wad) >= (int)sizeof(wad_arg)) return -1;
            DARG(wad_arg);
        }
#undef DARG
    }
    n = snprintf(args, sizeof(args), "{\"path\":");
    if (n < 0 || (size_t)n >= sizeof(args)) return -1;
    used = (size_t)n;
    if (json_quote(args, sizeof(args), &used, program)) return -1;
    n = snprintf(args + used, sizeof(args) - used, ",\"arg\":[%s],\"capture-output\":true", argv);
    if (n < 0 || (size_t)n >= sizeof(args) - used) return -1;
    used += (size_t)n;
    if (secret_request) {
        size_t length = strlen(job->password);
        unsigned char input[258];
        memcpy(input, job->password, length);
        input[length++] = '\n';
        n = b64_encode(input, length, encoded, sizeof(encoded));
        wipe(input, sizeof(input));
        if (n < 0 || used + 24 + (size_t)n >= sizeof(args)) {
            wipe(args, sizeof(args));
            wipe(encoded, sizeof(encoded));
            return -1;
        }
        n = snprintf(args + used, sizeof(args) - used, ",\"input-data\":\"%s\"", encoded);
        wipe(encoded, sizeof(encoded));
        if (n < 0 || (size_t)n >= sizeof(args) - used) {
            wipe(args, sizeof(args));
            return -1;
        }
        used += (size_t)n;
    }
    if (used + 2 >= sizeof(args)) {
        if (secret_request) wipe(args, sizeof(args));
        return -1;
    }
    args[used++] = '}'; args[used] = 0;
    n = request_begin(job, ST_EXEC, "guest-exec", args, 0);
    if (secret_request) wipe(args, sizeof(args));
    if (n) return -1;
    if (secret_request) {
        job->password_pending = 1;
    }
    return 0;
}

static int queue_exec_status(MgGuestJob *job)
{
    char args[96];
    int n = snprintf(args, sizeof(args), "{\"pid\":%llu}",
                     (unsigned long long)job->pid);
    if (n < 0 || (size_t)n >= sizeof(args)) return -1;
    return request_begin(job, ST_EXEC_STATUS, "guest-exec-status", args, 0);
}

static int classify_os(JParser *p, int info)
{
    static const char *fields[] = {"id", "name", "pretty-name", "variant", "kernel-release"};
    static const char *linuxes[] = {"linux", "ubuntu", "debian", "fedora", "red hat", "rhel",
        "centos", "rocky", "alpine", "arch linux", "opensuse", "linux mint",
        "gentoo", "manjaro", "elementary", "pop!_os", "kali"};
    char value[256], lower[256];
    size_t i, j;
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        int t = json_get(p, info, fields[i]);
        if (t < 0 || json_string(p, t, value, sizeof(value))) continue;
        for (j = 0; value[j] && j + 1 < sizeof(lower); j++) lower[j] = (char)tolower((unsigned char)value[j]);
        lower[j] = 0;
        if (strstr(lower, "windows")) return OS_WINDOWS;
        for (j = 0; j < sizeof(linuxes) / sizeof(linuxes[0]); j++) {
            if (strstr(lower, linuxes[j])) return OS_LINUX;
        }
        if (!strcmp(fields[i], "kernel-release") &&
            (strstr(lower, "linux") || lower[0] == '5' || lower[0] == '6' || lower[0] == '7')) return OS_LINUX;
    }
    return OS_ANY;
}

static int queue_osinfo(MgGuestJob *job)
{
    return request_begin(job, ST_OSINFO, "guest-get-osinfo", "{}", 0);
}

static int handle_response(MgGuestJob *job, JParser *parser, int root,
                           char *error, size_t capacity);

static int prepare_helper(MgGuestJob *job)
{
    const char *name;
    unsigned char random[16];
    char suffix[33];
    size_t i;
    static const char hex[] = "0123456789abcdef";
    if (mgp_random(random, sizeof(random))) return -1;
    for (i = 0; i < sizeof(random); i++) {
        suffix[i * 2] = hex[random[i] >> 4];
        suffix[i * 2 + 1] = hex[random[i] & 15];
    }
    suffix[sizeof(random) * 2] = 0;
    if (job->kind == JOB_DOOM) {
        name = "doom.sh";
        snprintf(job->guest_path, sizeof(job->guest_path), "/root/megas-doom-%s.sh", suffix);
    } else if (job->windows) {
        name = "windows-policy.ps1";
        snprintf(job->guest_path, sizeof(job->guest_path), "C:\\Windows\\Temp\\megas-windows-policy-%s.ps1", suffix);
    } else {
        name = "linux-policy.sh";
        snprintf(job->guest_path, sizeof(job->guest_path), "/root/megas-linux-policy-%s.sh", suffix);
    }
    wipe(random, sizeof(random));
    return load_helper(job, name);
}

static int start_file_upload(MgGuestJob *job)
{
    snprintf(job->progress, sizeof(job->progress),
             "Guest identity confirmed (%s); loading the packaged helper inside the VM.",
             job->windows ? "Windows" : "Linux");
    return queue_file_open(job);
}

static void sanitize_output(char *out, size_t capacity,
                            const unsigned char *bytes, size_t length)
{
    size_t i, used = 0;
    for (i = 0; i < length && used + 1 < capacity; i++) {
        unsigned char c = bytes[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c < 0x20 || c == 0x7f) continue;
        out[used++] = (char)c;
    }
    while (used && out[used - 1] == ' ') used--;
    out[used] = 0;
}

static void report_exec_failure(MgGuestJob *job, JParser *p, int result,
                                char *error, size_t capacity)
{
    unsigned char decoded[2048];
    char output[1536], encoded[4096];
    size_t length = 0;
    int token = json_get(p, result, "err-data");
    if (token < 0) token = json_get(p, result, "out-data");
    output[0] = 0;
    if (token >= 0 && !json_string(p, token, encoded, sizeof(encoded)) &&
        !b64_decode(encoded, strlen(encoded), decoded, sizeof(decoded), &length)) {
        sanitize_output(output, sizeof(output), decoded, length);
    }
    wipe(decoded, sizeof(decoded));
    wipe(encoded, sizeof(encoded));
    if (!output[0]) snprintf(output, sizeof(output), "Guest helper exited with a nonzero or abnormal status");
    (void)result;
    fail_job(job, error, capacity, output, 1);
}

static int response_ready(MgGuestJob *job, char *error, size_t capacity)
{
    int received = 0, n;
    char chunk[8192];
    while (received < 8) {
        n = mgp_recv(job->socket, chunk, sizeof(chunk));
        if (n == -2) break;
        if (n < 0 || n == 0) {
            fail_job(job, error, capacity,
                     "QEMU Guest Agent closed the persistent connection before a correlated response", 1);
            return -1;
        }
        received++;
        /* Process exactly the received bytes; a QGA stream can split at any
         * byte boundary, including between a JSON escape and its payload. */
        {
            int i;
            for (i = 0; i < n; i++) {
                unsigned char c = (unsigned char)chunk[i];
                if (job->stage == ST_SYNC && !job->sync_sentinel) {
                    if (c == 0xff) {
                        job->sync_sentinel = 1;
                        job->rx_used = 0;
                    }
                    continue;
                }
                if (c == 0xff) {
                    fail_job(job, error, capacity,
                             "Unexpected QEMU Guest Agent stream sentinel; connection synchronization was lost", 1);
                    return -1;
                }
                if (c == '\n') {
                    JParser parser;
                    uint64_t id;
                    int root, id_token;
                    while (job->rx_used && job->rx[job->rx_used - 1] == '\r') job->rx_used--;
                    job->rx[job->rx_used] = 0;
                    if (!job->rx_used) continue;
                    if (json_parse(&parser, job->rx, job->rx_used)) {
                        /* Stale complete lines are allowed before a matching
                         * response during sync. Else a bad frame is fatal. */
                        job->rx_used = 0;
                        if (job->stage == ST_SYNC) continue;
                        fail_job(job, error, capacity,
                                 "QEMU Guest Agent returned an invalid or oversized JSON message", 1);
                        return -1;
                    }
                    root = 0;
                    id_token = json_get(&parser, root, "id");
                    if (id_token < 0 || json_uint(&parser, id_token, &id) || id != job->request_id) {
                        job->rx_used = 0;
                        continue;
                    }
                    job->rx_used = 0;
                    if (handle_response(job, &parser, root, error, capacity)) return -1;
                    if (!job->active) return job->stage == ST_DONE ? 1 : -1;
                } else {
                    if (job->rx_used + 1 >= sizeof(job->rx)) {
                        fail_job(job, error, capacity,
                                 "QEMU Guest Agent message exceeded the 64 KiB protocol limit", 1);
                        return -1;
                    }
                    job->rx[job->rx_used++] = (char)c;
                }
            }
        }
    }
    return 0;
}

static int handle_response(MgGuestJob *job, JParser *p, int root,
                           char *error, size_t capacity)
{
    int result, error_token, t;
    uint64_t value;
    char detail[512];
    job->awaiting = 0;
    if (job->stage == ST_SYNC) {
        result = json_get(p, root, "return");
        if (result < 0 || json_uint(p, result, &value) ||
            value != job->sync_nonce) {
            fail_job(job, error, capacity,
                     "QEMU Guest Agent did not confirm the delimited synchronization nonce", 0);
            return -1;
        }
        job->synced = 1;
        if (queue_osinfo(job)) {
            fail_job(job, error, capacity, "Could not queue guest-get-osinfo", 0);
            return -1;
        }
        return 0;
    }
    error_token = json_get(p, root, "error");
    if (error_token >= 0) {
        t = json_get(p, error_token, "desc");
        detail[0] = 0;
        if (t >= 0) json_string(p, t, detail, sizeof(detail));
        if (!detail[0]) snprintf(detail, sizeof(detail), "QEMU Guest Agent rejected %s", job->stage == ST_OSINFO ? "guest-get-osinfo" : "the requested command");
        fail_job(job, error, capacity, detail, job->stage >= ST_FILE_OPEN);
        return -1;
    }
    result = json_get(p, root, "return");
    if (result < 0) {
        fail_job(job, error, capacity, "QEMU Guest Agent response did not include a return value", job->stage >= ST_FILE_OPEN);
        return -1;
    }
    switch (job->stage) {
    case ST_OSINFO:
        job->windows = classify_os(p, result) == OS_WINDOWS;
        if (!job->windows && classify_os(p, result) != OS_LINUX) {
            fail_job(job, error, capacity,
                     "guest-get-osinfo did not identify a supported Linux or Windows guest; no account change was attempted", 0);
            return -1;
        }
        if ((job->expected_os == OS_WINDOWS && !job->windows) ||
            (job->expected_os == OS_LINUX && job->windows)) {
            fail_job(job, error, capacity,
                     "guest-get-osinfo does not match the selected operating system; no guest account change was attempted", 0);
            return -1;
        }
        if (job->kind == JOB_DOOM && job->windows) {
            fail_job(job, error, capacity,
                     "Can it run Doom is supported only for a confirmed Linux guest; no helper was uploaded", 0);
            return -1;
        }
        if (prepare_helper(job)) {
            fail_job(job, error, capacity,
                     "The packaged guest helper is missing, empty, or exceeds the 24 KiB transfer bound", 0);
            return -1;
        }
        if (start_file_upload(job)) {
            fail_job(job, error, capacity, "Could not queue guest-file-open for the packaged helper", 0);
            return -1;
        }
        return 0;
    case ST_FILE_OPEN:
        if (json_uint(p, result, &job->file_handle) || !job->file_handle) {
            fail_job(job, error, capacity, "guest-file-open returned an invalid file handle", 1);
            return -1;
        }
        job->file_open = 1;
        if (queue_file_write(job)) {
            fail_job(job, error, capacity, "Could not queue the bounded guest helper write", 1);
            return -1;
        }
        return 0;
    case ST_FILE_WRITE:
        t = json_get(p, result, "count");
        if (json_uint(p, t, &value) || value != (job->payload_used - job->payload_sent)) {
            fail_job(job, error, capacity, "guest-file-write did not confirm the full helper chunk", 1);
            return -1;
        }
        job->payload_sent += (size_t)value;
        if (job->payload_sent < job->payload_used) {
            if (queue_file_write(job)) {
                fail_job(job, error, capacity, "Could not queue the next bounded guest helper chunk", 1);
                return -1;
            }
        } else if (queue_file_close(job)) {
            fail_job(job, error, capacity, "Could not queue guest-file-close", 1);
            return -1;
        }
        return 0;
    case ST_FILE_CLOSE:
        job->file_open = 0;
        if (queue_exec(job)) {
            fail_job(job, error, capacity, "Could not queue fixed-argv guest-exec for the packaged helper", 1);
            return -1;
        }
        return 0;
    case ST_EXEC:
        t = json_get(p, result, "pid");
        if (json_uint(p, t, &job->pid) || !job->pid) {
            fail_job(job, error, capacity, "guest-exec did not return a valid helper PID", 1);
            return -1;
        }
        job->stage = ST_EXEC_STATUS;
        job->next_poll = mgp_now() + QGA_POLL_MS;
        snprintf(job->progress, sizeof(job->progress),
                 "%s helper is running in the confirmed guest (process PID %llu); waiting for its exit status.",
                 job->kind == JOB_DOOM ? "Doom" : "Guest policy",
                 (unsigned long long)job->pid);
        if (job->password_pending) {
            wipe(job->password, sizeof(job->password));
            job->password_pending = 0;
        }
        return 0;
    case ST_EXEC_STATUS:
        t = json_get(p, result, "exited");
        if (t < 0) {
            fail_job(job, error, capacity, "guest-exec-status response omitted its exited state", 1);
            return -1;
        }
        if (!json_true(p, t)) {
            job->next_poll = mgp_now() + QGA_POLL_MS;
            snprintf(job->progress, sizeof(job->progress),
                     "%s helper is still running in the guest (process PID %llu); checking again.",
                     job->kind == JOB_DOOM ? "Doom" : "Guest policy",
                     (unsigned long long)job->pid);
            job->uncertain = 0;
            return 0;
        }
        t = json_get(p, result, "exitcode");
        if (json_uint(p, t, &value) || value != 0) {
            report_exec_failure(job, p, result, error, capacity);
            return -1;
        }
        detail[0] = 0;
        t = json_get(p, result, "out-data");
        if (t >= 0) {
            char encoded[4096];
            unsigned char decoded[2048];
            size_t n = 0;
            if (!json_string(p, t, encoded, sizeof(encoded)) &&
                !b64_decode(encoded, strlen(encoded), decoded, sizeof(decoded), &n)) {
                sanitize_output(detail, sizeof(detail), decoded, n);
            }
            wipe(encoded, sizeof(encoded)); wipe(decoded, sizeof(decoded));
        }
        if (job->kind == JOB_DOOM) {
            snprintf(job->progress, sizeof(job->progress), "%s",
                     detail[0] ? detail : "Doom helper exited successfully; verify the live guest screen.");
        } else {
            snprintf(job->progress, sizeof(job->progress), "%s",
                     detail[0] ? detail : "Guest account policy helper completed with exit status 0.");
        }
        finish_job(job);
        return 0;
    default:
        fail_job(job, error, capacity, "QEMU Guest Agent response arrived in an invalid operation stage", 1);
        return -1;
    }
}

static int begin_job(MgGuestJob *job, unsigned port, unsigned kind,
                     const char *username, const char *helper_dir,
                     char *error, size_t capacity)
{
    const char *user = username && *username ? username : "megas";
    if (!job) {
        if (capacity) snprintf(error, capacity, "Copy this to your agent: Guest job storage is missing; no guest command ran.");
        return -1;
    }
    memset(job, 0, sizeof(*job));
    job->socket = MGP_BAD_SOCKET;
    if (port < 1 || port > 65535 || !valid_username(user) ||
        !valid_utf8_text(helper_dir, 1023, 0)) {
        if (capacity) snprintf(error, capacity,
            "Copy this to your agent: Repair the Megas guest request. Choose a valid loopback QGA port, non-administrator guest username, and packaged helper folder; no guest command ran.");
        return -1;
    }
    job->port = port;
    job->kind = kind;
    snprintf(job->username, sizeof(job->username), "%s", user);
    snprintf(job->helpers, sizeof(job->helpers), "%s", helper_dir);
    return start_connection(job, error, capacity);
}

int mg_guest_policy_start(MgGuestJob *job, unsigned port, const char *os,
                          int enabled, const char *username, const char *password,
                          const char *helper_dir, char *error, size_t capacity)
{
    size_t n;
    int expectation;
    if (capacity) error[0] = 0;
    if (begin_job(job, port, JOB_POLICY, username, helper_dir, error, capacity)) return -1;
    expectation = expected_os(os);
    if (expectation < 0) {
        fail_job(job, error, capacity,
                 "The selected operating system must be identified as Linux or Windows", 0);
        return -1;
    }
    job->expected_os = expectation;
    job->enabled = !!enabled;
    if (password) {
        n = strlen(password);
        if (n >= sizeof(job->password) || !valid_utf8_text(password, sizeof(job->password) - 1, 1)) {
            fail_job(job, error, capacity,
                     "Password must be at most 256 bytes and contain no control characters; no guest command ran", 0);
            return -1;
        }
        if (job->enabled && n) {
            fail_job(job, error, capacity,
                     "Choose either passwordless mode or an explicit guest password, not both; no guest command ran", 0);
            return -1;
        }
        memcpy(job->password, password, n + 1);
    }
    return 0;
}

int mg_guest_doom_start(MgGuestJob *job, unsigned port, const char *helper_dir,
                        const char *wad, char *error, size_t capacity)
{
    if (capacity) error[0] = 0;
    if (begin_job(job, port, JOB_DOOM, "megas", helper_dir, error, capacity)) return -1;
    if (wad && *wad) {
        if (wad[0] != '/' || !valid_utf8_text(wad, sizeof(job->wad) - 1, 0)) {
            fail_job(job, error, capacity,
                     "WAD path must be an absolute, printable path inside the Linux guest; no command ran", 0);
            return -1;
        }
        snprintf(job->wad, sizeof(job->wad), "%s", wad);
    }
    job->enabled = 1;
    return 0;
}

void mg_guest_clear(MgGuestJob *job)
{
    if (!job) return;
    if (job->active && job->port && job->port <= 65535 &&
        job->stage >= ST_SYNC && job->stage <= ST_EXEC_STATUS) close_socket(job);
    wipe(job, sizeof(*job));
    job->socket = MGP_BAD_SOCKET;
}

int mg_guest_poll(MgGuestJob *job, char *error, size_t capacity)
{
    int n, sent;
    uint64_t now;
    if (capacity) error[0] = 0;
    if (!job) {
        if (capacity) snprintf(error, capacity, "Copy this to your agent: Guest job storage is missing.");
        return -1;
    }
    if (job->stage == ST_DONE) return 1;
    if (job->stage == ST_FAILED || !job->active || job->socket == MGP_BAD_SOCKET) return -1;
    now = mgp_now();
    if (job->awaiting && now >= job->deadline) {
        fail_job(job, error, capacity,
                 "QEMU Guest Agent response timed out; the controller will not retry an unconfirmed guest mutation", 1);
        return -1;
    }
    if (!job->awaiting && job->stage == ST_EXEC_STATUS && now >= job->next_poll) {
        if (queue_exec_status(job)) {
            fail_job(job, error, capacity, "Could not queue guest-exec-status", 1);
            return -1;
        }
    }
    if (job->tx_sent < job->tx_used) {
        sent = mgp_send(job->socket, job->tx + job->tx_sent,
                        job->tx_used - job->tx_sent);
        if (sent == -2) return 0;
        if (sent <= 0) {
            fail_job(job, error, capacity,
                     "QEMU Guest Agent connection failed while sending a bounded request", 1);
            return -1;
        }
        job->tx_sent += (size_t)sent;
        if (job->tx_sent == job->tx_used) {
            if (job->password_pending && job->stage == ST_EXEC) {
                /* Request bytes contain only the base64 representation and
                 * are scrubbed immediately after the final send completes. */
                wipe(job->tx, sizeof(job->tx));
                job->tx_used = job->tx_sent = 0;
            }
            return 0;
        }
        return 0;
    }
    n = response_ready(job, error, capacity);
    if (n < 0) return -1;
    if (job->stage == ST_EXEC_STATUS && !job->awaiting &&
        mgp_now() >= job->next_poll) {
        if (queue_exec_status(job)) {
            fail_job(job, error, capacity, "Could not queue guest-exec-status", 1);
            return -1;
        }
    }
    return job->stage == ST_DONE ? 1 : 0;
}

