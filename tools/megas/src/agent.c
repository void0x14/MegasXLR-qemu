/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native stdio adapter. No frame capture loop or model SDK lives here. */
#include "megas.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#define AGENT_LINE_MAX 65536
#define AGENT_REPLY_MAX 65536
#define AGENT_IMAGE_MAX (20u * 1024u * 1024u)
typedef struct { const char *a, *b; } Span;

static const char *space(const char *p)
{
    while (*p && isspace((unsigned char)*p)) { p++; }
    return p;
}

static const char *string_end(const char *p)
{
    if (*p++ != '"') { return NULL; }
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') { return p; }
        if (c < 32) { return NULL; }
        if (c == '\\') {
            unsigned i;
            c = (unsigned char)*p++;
            if (!c) { return NULL; }
            if (c == 'u') {
                for (i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)*p++)) { return NULL; }
                }
            } else if (!strchr("\"\\/bfnrt", c)) { return NULL; }
        }
    }
    return NULL;
}

static const char *value_end(const char *p, unsigned depth)
{
    const char *q;
    if (depth > 24) { return NULL; }
    p = space(p);
    if (*p == '"') { return string_end(p); }
    if (*p == '{' || *p == '[') {
        char close = *p++ == '{' ? '}' : ']';
        int object = close == '}';
        p = space(p);
        if (*p == close) { return p + 1; }
        for (;;) {
            if (object) {
                p = string_end(p);
                if (!p || *(p = space(p)) != ':') { return NULL; }
                p++;
            }
            p = value_end(p, depth + 1);
            if (!p) { return NULL; }
            p = space(p);
            if (*p == close) { return p + 1; }
            if (*p++ != ',') { return NULL; }
            p = space(p);
        }
    }
    if (!strncmp(p, "true", 4)) { return p + 4; }
    if (!strncmp(p, "false", 5)) { return p + 5; }
    if (!strncmp(p, "null", 4)) { return p + 4; }
    q = p;
    if (*p == '-') { p++; }
    if (*p == '0') { p++; }
    else {
        if (*p < '1' || *p > '9') { return NULL; }
        while (isdigit((unsigned char)*p)) { p++; }
    }
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) { return NULL; }
        while (isdigit((unsigned char)*p)) { p++; }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') { p++; }
        if (!isdigit((unsigned char)*p)) { return NULL; }
        while (isdigit((unsigned char)*p)) { p++; }
    }
    return p == q ? NULL : p;
}

static int property(Span object, const char *key, Span *out)
{
    const char *p = space(object.a), *k, *end, *v;
    size_t n = strlen(key);
    if (*p++ != '{') { return 0; }
    while (*(p = space(p)) && p < object.b && *p != '}') {
        k = p;
        end = string_end(p);
        if (!end || *(p = space(end)) != ':') { return 0; }
        v = space(p + 1);
        p = value_end(v, 0);
        if (!p || p > object.b) { return 0; }
        if ((size_t)(end - k) == n + 2 && !memcmp(k + 1, key, n)) {
            out->a = v;
            out->b = p;
            return 1;
        }
        p = space(p);
        if (*p == '}') { break; }
        if (*p++ != ',') { return 0; }
    }
    return 0;
}

static int equals(Span s, const char *text)
{
    size_t n = strlen(text);
    return s.b - s.a == (ptrdiff_t)n + 2 && *s.a == '"' &&
           !memcmp(s.a + 1, text, n);
}

static int decode(Span s, char *out, size_t cap)
{
    const char *p = s.a + 1;
    size_t n = 0;
    if (*s.a != '"' || s.b <= p || s.b[-1] != '"') { return 0; }
    while (p < s.b - 1) {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': return 0; /* Engine paths are serialized as UTF-8. */
            default: break;
            }
        }
        if (!c || n + 1 >= cap) { return 0; }
        out[n++] = c;
    }
    out[n] = 0;
    return 1;
}

static void quoted(const char *s)
{
    putchar('"');
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        switch (c) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c < 32) { printf("\\u%04x", c); }
            else { putchar(c); }
        }
    }
    putchar('"');
}

static void prefix(Span id)
{
    fputs("{\"jsonrpc\":\"2.0\",\"id\":", stdout);
    if (id.a) { fwrite(id.a, 1, (size_t)(id.b - id.a), stdout); }
    else { fputs("null", stdout); }
}

static void protocol_error(Span id, int code, const char *message)
{
    prefix(id);
    printf(",\"error\":{\"code\":%d,\"message\":", code);
    quoted(message);
    fputs("}}\n", stdout);
    fflush(stdout);
}

/* Stream base64 from disk: never allocate a second image or an encoded copy. */
static void image_content(FILE *f)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char buf[3];
    size_t n;
    fputs(",{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"", stdout);
    while ((n = fread(buf, 1, sizeof(buf), f)) != 0) {
        putchar(b64[buf[0] >> 2]);
        putchar(b64[((buf[0] & 3) << 4) | (n > 1 ? buf[1] >> 4 : 0)]);
        putchar(n > 1 ? b64[((buf[1] & 15) << 2) |
                           (n > 2 ? buf[2] >> 6 : 0)] : '=');
        putchar(n > 2 ? b64[buf[2] & 63] : '=');
    }
    fputs("\"}", stdout);
}

static FILE *open_image(const char *reply)
{
    Span all = { reply, reply + strlen(reply) }, path;
    char filename[1024];
    unsigned char magic[8];
    long size;
    FILE *f;
    if (!property(all, "image_path", &path) ||
        !decode(path, filename, sizeof(filename)) || !filename[0]) { return NULL; }
    f = mgp_fopen(filename, "rb");
    if (!f) { return NULL; }
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, "\211PNG\r\n\032\n", 8) ||
        fseek(f, 0, SEEK_END) || (size = ftell(f)) < 8 ||
        (unsigned long)size > AGENT_IMAGE_MAX || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    return f;
}

static int unsigned_span(Span value, unsigned long long *result)
{
    const char *p = value.a;
    unsigned long long n = 0;
    if (!p || p >= value.b || !isdigit((unsigned char)*p)) return -1;
    for (; p < value.b; p++) {
        unsigned digit;
        if (!isdigit((unsigned char)*p)) return -1;
        digit = (unsigned)(*p - '0');
        if (n > (ULLONG_MAX - digit) / 10) return -1;
        n = n * 10 + digit;
    }
    *result = n;
    return 0;
}

/* Poll the owner for at most 1.5 seconds. The owner runs OCR as an isolated
 * bounded child, so this wait never blocks its GUI/event loop. */
static int auto_ocr(unsigned port, const char *reply, char *text, size_t text_cap,
                    char *message, size_t message_cap, unsigned long long *generation)
{
    char request[128], owner_reply[AGENT_REPLY_MAX];
    Span all = { reply, reply + strlen(reply) }, target_span, state, ocr, status, value, image_gen, ocr_gen;
    unsigned long long target;
    uint64_t deadline;
    int calls = 0;
    text[0] = message[0] = 0;
    if (!property(all, "image_generation", &target_span) ||
        unsigned_span(target_span, &target)) {
        snprintf(message, message_cap, "No current live image generation is available; OCR was not run.");
        return 0;
    }
    *generation = target;
    snprintf(request, sizeof(request), "{\"action\":\"ocr\",\"generation\":%llu}", target);
    if (mg_client(port, request, owner_reply, sizeof(owner_reply))) {
        snprintf(message, message_cap, "OCR request could not reach the native owner; only controller metadata is available.");
        return 0;
    }
    deadline = mgp_now() + 1500;
    do {
        Span current = { owner_reply, owner_reply + strlen(owner_reply) };
        if (property(current, "state", &state) && property(current, "ocr", &ocr) &&
            property(ocr, "status", &status) && property(current, "image_generation", &image_gen) &&
            property(ocr, "generation", &ocr_gen)) {
            unsigned long long image_number = 0, ocr_number = 0;
            if (!unsigned_span(image_gen, &image_number) && !unsigned_span(ocr_gen, &ocr_number) &&
                image_number == target && ocr_number == target) {
                if (equals(status, "complete")) {
                    if (property(ocr, "text", &value) && decode(value, text, text_cap)) {
                        if (property(ocr, "message", &value)) (void)decode(value, message, message_cap);
                        return 1;
                    }
                    snprintf(message, message_cap, "OCR completed, but its text result exceeded the adapter limit.");
                    return 0;
                }
                if (!equals(status, "running")) {
                    if (property(ocr, "message", &value) && decode(value, message, message_cap)) return 0;
                    snprintf(message, message_cap, "OCR is unavailable or did not complete; no screen text was inferred.");
                    return 0;
                }
            } else {
                snprintf(message, message_cap, "The display changed during OCR; extracted text was discarded as stale.");
                return 0;
            }
        }
        if (mgp_now() >= deadline) break;
        mgp_sleep(100);
        calls++;
        if (calls > 15) break;
        if (mg_client(port, "{\"action\":\"status\"}", owner_reply, sizeof(owner_reply))) break;
    } while (mgp_now() < deadline);
    snprintf(message, message_cap, "OCR is still running or unavailable within the 1.5-second adapter wait; no screen text was inferred.");
    return 0;
}

static void tool_result(unsigned port, Span id, const char *reply, int vision, int failure)
{
    Span all = { reply, reply + strlen(reply) }, ok;
    FILE *f = open_image(reply);
    char ocr_text[8192] = {0}, ocr_message[512] = {0}, combined[8704];
    unsigned long long generation = 0;
    int has_ocr = vision == 2 && f && auto_ocr(port, reply, ocr_text, sizeof(ocr_text),
                                ocr_message, sizeof(ocr_message), &generation);
    if (property(all, "ok", &ok) && ok.b - ok.a == 5 &&
        !memcmp(ok.a, "false", 5)) { failure = 1; }
    prefix(id);
    fputs(",\"result\":{\"content\":[{\"type\":\"text\",\"text\":", stdout);
    quoted(reply);
    putchar('}');
    if (has_ocr) {
        snprintf(combined, sizeof(combined), "Tesseract OCR for live image generation %llu:\n%s",
                 generation, ocr_text);
        fputs(",{\"type\":\"text\",\"text\":", stdout);
        quoted(combined);
        putchar('}');
    } else if (f && vision == 2) {
        fputs(",{\"type\":\"text\",\"text\":", stdout);
        quoted(ocr_message[0] ? ocr_message : "OCR unavailable; controller metadata is not screen text.");
        putchar('}');
    } else if (!f) {
        fputs(",{\"type\":\"text\",\"text\":\"No current image is available, so OCR was not run. Screen text has not been OCR-decoded; the response contains controller metadata only.\"}", stdout);
    }
    if (vision != 2 && f) { image_content(f); }
    if (f) fclose(f);
    printf("],\"isError\":%s}}\n", failure ? "true" : "false");
    fflush(stdout);
}

static void list_tools(Span id)
{
    prefix(id);
    fputs(",\"result\":{\"tools\":[{\"name\":\"megas_vm\",\"title\":\"Control the VM\","
          "\"description\":\"Native VM state and livestream discovery. Android uses android_observe and android_act with provider-issued target, epoch and expected_revision. Never guess coordinates. Queued means accepted for transport, not completed. Inspect pending_action_id/outcome and independent task effects. Screenshots and OCR are explicit options; the default adapter does neither.\","
          "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
          "\"action\":{\"type\":\"string\",\"description\":\"start, status, android_observe, android_act, android_ack_uncertain, pause, resume, stop, reset, screenshot, checkpoint, restore, steps, ocr, repair; non-Android guests also support key, click, type, resize, password, doom, harness\"},"
          "\"android\":{\"type\":\"boolean\"},\"include_image\":{\"type\":\"boolean\",\"default\":false},"
          "\"target\":{\"type\":\"string\"},\"epoch\":{\"type\":\"string\"},\"expected_revision\":{\"type\":\"integer\"},"
          "\"operation\":{\"type\":\"string\",\"enum\":[\"click\",\"long_click\",\"set_text\",\"scroll_forward\",\"scroll_backward\",\"focus\"]},"
          "\"disk\":{\"type\":\"string\"},\"iso\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},"
          "\"disk_bus\":{\"type\":\"string\",\"enum\":[\"virtio\",\"ide\"]},"
          "\"video_adapter\":{\"type\":\"string\",\"enum\":[\"virtio\",\"vga\"]},"
          "\"boot_source\":{\"type\":\"string\",\"enum\":[\"auto\",\"iso\",\"disk\"]},"
          "\"cpus\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4},\"cpu_percent\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":400},"
          "\"accel\":{\"type\":\"string\",\"enum\":[\"auto\",\"kvm\",\"whpx\",\"hvf\",\"tcg\"]},"
          "\"ram_mb\":{\"type\":\"integer\",\"minimum\":16,\"maximum\":32768},"
          "\"max_threads\":{\"type\":\"integer\",\"minimum\":8,\"maximum\":128},"
          "\"key\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"}},"
          "\"required\":[\"action\"],\"additionalProperties\":true}}]}}\n", stdout);
    fflush(stdout);
}

int mg_agent_stdio(unsigned port, int vision)
{
    char *line = malloc(AGENT_LINE_MAX), *reply = malloc(AGENT_REPLY_MAX);
    char *request = malloc(AGENT_LINE_MAX);
    int initialized = 0;
    if (!line || !reply || !request) {
        free(line); free(reply); free(request); return 1;
    }
    while (fgets(line, AGENT_LINE_MAX, stdin)) {
        Span all, id = {0}, method, params, name, args, version;
        const char *end;
        size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(stdin)) {
            int c;
            while ((c = getchar()) != EOF && c != '\n') { }
            protocol_error(id, -32700, "Request exceeds the bounded input limit.");
            continue;
        }
        all.a = space(line);
        end = value_end(all.a, 0);
        if (!end || *space(end) || *all.a != '{') {
            protocol_error(id, -32700, "Invalid JSON; send one JSON object per line.");
            continue;
        }
        all.b = end;
        property(all, "id", &id);
        if (id.a && *id.a != '"' && *id.a != '-' &&
            !isdigit((unsigned char)*id.a) && !equals(id, "null")) {
            if (!(id.b - id.a == 4 && !memcmp(id.a, "null", 4))) {
                id.a = NULL;
                protocol_error(id, -32600, "Request id must be a number, string or null.");
                continue;
            }
        }
        if (!property(all, "method", &method)) {
            protocol_error(id, -32600, "A method is required.");
            continue;
        }
        if (!id.a) { continue; } /* Notifications have no response. */
        if (equals(method, "initialize")) {
            const char *selected = "2025-06-18";
            if (property(all, "params", &params) &&
                property(params, "protocolVersion", &version) &&
                (equals(version, "2024-11-05") || equals(version, "2025-03-26"))) {
                selected = equals(version, "2024-11-05") ?
                           "2024-11-05" : "2025-03-26";
            }
            prefix(id);
            printf(",\"result\":{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"MegasXLR\",\"version\":\"0.1.0\"},\"instructions\":\"Use semantic state plus the independently exposed continuous livestream. PNG and OCR are optional. Never ignore pending, failed or stale state; never guess Android coordinates.\"}}\n", selected);
            initialized = 1;
            fflush(stdout);
        } else if (equals(method, "ping")) {
            prefix(id); fputs(",\"result\":{}}\n", stdout); fflush(stdout);
        } else if (!initialized) {
            protocol_error(id, -32000, "Initialize the native adapter first.");
        } else if (equals(method, "tools/list")) {
            list_tools(id);
        } else if (equals(method, "tools/call")) {
            if (!property(all, "params", &params) ||
                !property(params, "name", &name) || !equals(name, "megas_vm") ||
                !property(params, "arguments", &args) || *args.a != '{') {
                protocol_error(id, -32602, "Use megas_vm with an object of action arguments.");
                continue;
            }
            n = (size_t)(args.b - args.a);
            if(n+32>=AGENT_LINE_MAX){protocol_error(id,-32602,"Action exceeds the bounded input limit.");continue;}
            memcpy(request, args.a, n); request[n] = 0;
            {Span image,action;
             int explicit_capture=property(args,"action",&action)&&(equals(action,"screenshot")||equals(action,"ocr"));
             if(!vision&&!explicit_capture&&!property(args,"include_image",&image)){
                 snprintf(request+n-1,AGENT_LINE_MAX-n+1,"%s\"include_image\":false}",n>2?",":"");
             }
            }
            reply[0] = 0;
            {
                int failure = mg_client(port, request, reply, AGENT_REPLY_MAX);
                if (failure && !reply[0]) {
                    snprintf(reply, AGENT_REPLY_MAX,
                             "{\"ok\":false,\"image_path\":\"\","
                             "\"feedback\":\"The VM manager is unavailable.\","
                             "\"repair_prompt\":\"Inspect the visible MegasXLR manager and its session journal. Restore its authenticated native control connection; keep existing VM disks and checkpoints.\"}");
                }
                tool_result(port, id, reply, vision, failure != 0);
            }
        } else {
            protocol_error(id, -32601, "Unsupported native adapter method.");
        }
    }
    free(reply); free(line); free(request);
    return ferror(stdin) || ferror(stdout) ? 1 : 0;
}
