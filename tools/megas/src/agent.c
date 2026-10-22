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
 * bounded child, so this wait never blocks its GUI/event loop.
 *
 * The owner captures the frame itself when no generation is named, so a
 * scrolling screen cannot invalidate the request before OCR starts. A screen
 * that animates (a blinking cursor is enough) still advances its generation
 * while Tesseract works; that text belongs to the generation it was captured
 * from, so it is returned with that generation and flagged as stale instead of
 * being thrown away. Text for an older generation than the caller was shown is
 * never presented as the current screen. */
static int auto_ocr(unsigned port, const char *reply, char *text, size_t text_cap,
                    char *message, size_t message_cap, unsigned long long *generation,
                    int *stale)
{
    char owner_reply[AGENT_REPLY_MAX];
    Span all = { reply, reply + strlen(reply) }, target_span;
    unsigned long long target;
    uint64_t deadline;
    int calls = 0;
    text[0] = message[0] = 0;
    if (stale) { *stale = 0; }
    if (!property(all, "image_generation", &target_span) ||
        unsigned_span(target_span, &target)) {
        snprintf(message, message_cap, "No current live image generation is available; OCR was not run.");
        return 0;
    }
    *generation = target;
    if (mg_client(port, "{\"action\":\"ocr\"}", owner_reply, sizeof(owner_reply))) {
        /* A refused request still carries the owner's own reason, which is more
         * useful than calling it unreachable. */
        Span refused = { owner_reply, owner_reply + strlen(owner_reply) }, part, value;
        if (property(refused, "ocr", &part) && property(part, "message", &value) &&
            decode(value, message, message_cap) && message[0]) return 0;
        if (property(refused, "fallback_text", &value) &&
            decode(value, message, message_cap) && message[0]) return 0;
        snprintf(message, message_cap, "OCR request could not reach the native owner; only controller metadata is available.");
        return 0;
    }
    deadline = mgp_now() + 1500;
    for (;;) {
        Span current = { owner_reply, owner_reply + strlen(owner_reply) };
        Span state, ocr, status, value, ocr_gen;
        unsigned long long ocr_number = 0;
        int usable = 0;
        if (property(current, "state", &state) && property(current, "ocr", &ocr) &&
            property(ocr, "status", &status) && property(ocr, "generation", &ocr_gen) &&
            !unsigned_span(ocr_gen, &ocr_number)) {
            usable = property(ocr, "text", &value) && decode(value, text, text_cap) && text[0];
            if ((equals(status, "complete") || equals(status, "stale_image")) &&
                usable && ocr_number >= target) {
                *generation = ocr_number;
                if (property(ocr, "message", &value)) (void)decode(value, message, message_cap);
                if (stale && equals(status, "stale_image")) { *stale = 1; }
                return 1;
            }
            if (!equals(status, "running")) {
                if (property(ocr, "message", &value) && decode(value, message, message_cap) && message[0]) return 0;
                snprintf(message, message_cap, "OCR is unavailable or did not complete; no screen text was inferred.");
                return 0;
            }
            if (usable && ocr_number < target) {
                snprintf(message, message_cap, "The owner still reports text for an older image generation; no text was inferred for this screen.");
                return 0;
            }
        }
        if (mgp_now() >= deadline) break;
        mgp_sleep(100);
        calls++;
        if (calls > 15) break;
        if (mg_client(port, "{\"action\":\"status\"}", owner_reply, sizeof(owner_reply))) break;
    }
    snprintf(message, message_cap, "OCR is still running or unavailable within the 1.5-second adapter wait; no screen text was inferred.");
    return 0;
}

static void tool_result(unsigned port, Span id, const char *reply, int vision, int failure)
{
    Span all = { reply, reply + strlen(reply) }, ok;
    FILE *f = open_image(reply);
    char ocr_text[8192] = {0}, ocr_message[512] = {0}, combined[8704];
    unsigned long long generation = 0;
    int stale = 0;
    int has_ocr = f && auto_ocr(port, reply, ocr_text, sizeof(ocr_text),
                                ocr_message, sizeof(ocr_message), &generation, &stale);
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
        if (stale) {
            char note[160];
            snprintf(note, sizeof(note),
                     "The display advanced after this capture; the text above describes image generation %llu, not the newest frame.",
                     generation);
            fputs(",{\"type\":\"text\",\"text\":", stdout);
            quoted(note);
            putchar('}');
        }
    } else if (f) {
        fputs(",{\"type\":\"text\",\"text\":", stdout);
        quoted(ocr_message[0] ? ocr_message : "OCR unavailable; controller metadata is not screen text.");
        putchar('}');
    } else {
        fputs(",{\"type\":\"text\",\"text\":\"No current image is available, so OCR was not run. Screen text has not been OCR-decoded; the response contains controller metadata only.\"}", stdout);
    }
    if (vision && f) { image_content(f); }
    if (f) fclose(f);
    printf("],\"isError\":%s}}\n", failure ? "true" : "false");
    fflush(stdout);
}

static void list_tools(Span id)
{
    prefix(id);
    fputs(",\"result\":{\"tools\":[{\"name\":\"megas_vm\",\"title\":\"Control the VM\",\"description\":\"Native VM control. Every result includes actual live state and the latest display image when available. Default agent mode returns PNG plus bounded Tesseract OCR text when OCR is available; --text-only returns actual OCR text or an honest unavailable message. Commands are audited; a confirmed checkpoint can be restored. Run preflight for an existing disk before start and reuse its recorded RAM, bus and video: the VM's memory and disk controller belong to the VM, so start is refused while they are unknown and the refusal names what to ask the user. Never silently change them. Use status to inspect and repair for the forensic repair prompt. Never infer success from a requested action.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"preflight, start, status, pause, resume, stop, reset, key, click, type, resize, screenshot, checkpoint, restore, steps, password, doom, ocr, harness, repair\"},\"disk\":{\"type\":\"string\",\"description\":\"VM disk image path. For an existing disk run preflight first and reuse the recorded RAM and bus.\"},\"ram_mb\":{\"type\":\"integer\",\"minimum\":16,\"maximum\":32768,\"description\":\"VM RAM in MiB. Use the memory the VM already has, from preflight. Change it only when the user asks or the task needs more memory; there is no default.\"},\"bus\":{\"type\":\"string\",\"enum\":[\"ide\",\"virtio\",\"scsi\",\"nvme\"],\"description\":\"Disk controller bus. It must match the VM's existing disk; an existing disk is never reattached to a different bus.\"},\"video\":{\"type\":\"string\",\"description\":\"Display device, for example VGA or virtio-vga. Reuse the VM's existing device unless the user asks to change it.\"},\"iso\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"},\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"enabled\":{\"type\":\"boolean\"},\"username\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"wad\":{\"type\":\"string\"}},\"required\":[\"action\"],\"additionalProperties\":true}}]}}\n", stdout);
    fflush(stdout);
}

/* A harness launches this adapter and nothing else, so the adapter owns the
 * lifecycle of the controller it speaks to. When no owner answers on the
 * configured port, the adapter starts one in this session's runtime folder
 * and retries the action once. The owner is detached: it outlives the adapter,
 * so a VM keeps running across harness restarts, and a later adapter reuses
 * it instead of starting a second one (the owner lock admits one). */
static int escape_reply(char *out, size_t capacity, const char *text)
{
    size_t n = 0;
    if (!capacity) return -1;
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            if (n + 2 >= capacity) return -1;
            out[n++] = '\\'; out[n++] = (char)c;
        } else if (c < 32) {
            if (n + 6 >= capacity) return -1;
            n += (size_t)snprintf(out + n, capacity - n, "\\u%04x", c);
        } else {
            if (n + 1 >= capacity) return -1;
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
    return 0;
}

static int owner_answers(unsigned port)
{
    /* A status answer is a few kilobytes of verified state; a smaller reply
     * buffer truncates it and turns every successful probe into a parse
     * failure. */
    static char probe[AGENT_REPLY_MAX];
    return mg_client(port, "{\"action\":\"status\"}", probe, sizeof(probe)) == 0;
}

static int owner_wants_gui(void)
{
    char mode[32], display[256];
    if (!mgp_env("MEGAS_OWNER_MODE", mode, sizeof(mode))) {
        if (!strcmp(mode, "serve")) return 0;
        if (!strcmp(mode, "gui")) return 1;
    }
    if (!mgp_env("DISPLAY", display, sizeof(display)) && display[0]) return 1;
    if (!mgp_env("WAYLAND_DISPLAY", display, sizeof(display)) && display[0]) return 1;
    return 0;
}

static int owner_start(unsigned port, char *note, size_t capacity)
{
    const char *commands[2];
    MgConfig cfg;
    MgpProcess process;
    char self[1024], log[1200], port_text[16], error[512], *argv[8];
    unsigned count = 0, attempt;
    mg_config_defaults(&cfg);
    snprintf(log, sizeof(log), "%.900s/owner-adapter.log", cfg.runtime_dir);
    if (mgp_self_path(self, sizeof(self))) {
        snprintf(note, capacity, "The adapter could not resolve its own executable path.");
        return -1;
    }
    if (mgp_mkdir(cfg.runtime_dir)) {
        snprintf(note, capacity, "The runtime folder %.300s could not be created.", cfg.runtime_dir);
        return -1;
    }
    if (owner_wants_gui()) commands[count++] = "gui";
    commands[count++] = "serve";
    snprintf(port_text, sizeof(port_text), "%u", port);
    argv[0] = self; argv[1] = "--runtime"; argv[2] = cfg.runtime_dir;
    argv[3] = "--port"; argv[4] = port_text; argv[5] = NULL; argv[6] = NULL;

    for (attempt = 0; attempt < count; attempt++) {
        char token_path[1024];
        unsigned waited;
        snprintf(token_path, sizeof(token_path), "%.900s/session-%u.token", cfg.runtime_dir, port);
        argv[5] = (char *)commands[attempt];
        if (mgp_spawn_detached(&process, argv, log, error, sizeof(error))) {
            snprintf(note, capacity, "The native owner could not start: %.260s", error);
            continue;
        }
        /* Waiting for the session credential first keeps the journal free of
         * probes that could only answer "no owner yet". */
        for (waited = 0; waited < 120; waited++) {
            if (mgp_file_size(token_path) == 64 && owner_answers(port)) return 1;
            if (!mgp_alive(&process)) break;
            mgp_sleep(50);
        }
        mgp_reap(&process);
        if (waited == 120) {
            snprintf(note, capacity,
                     "A native owner is running but did not answer within six seconds; %.400s holds its output.", log);
            return -1;
        }
        /* That owner exited before answering: try the next one. */
    }
    snprintf(note, capacity, "No native owner could be started; %.400s holds the owner output.", log);
    return -1;
}

/* mg_client states a missing owner in its own words; only those two failures
 * mean "start one", never a refused or unanswered action. */
static int owner_missing(const char *reply)
{
    return !reply[0] || strstr(reply, "Controller is not running") ||
           strstr(reply, "controller is unreachable");
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
            printf(",\"result\":{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"MegasXLR\",\"version\":\"0.1.0\"},\"instructions\":\"Current VM state and image accompany every VM action. Read them before the next action; never ignore failed or stale state. For an existing disk call preflight first: reuse the recorded RAM, disk bus and video. Do not change RAM or bus unless the user explicitly asks or the task requires it.\"}}\n", selected);
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
            memcpy(request, args.a, n); request[n] = 0;
            reply[0] = 0;
            {
                int failure = mg_client(port, request, reply, AGENT_REPLY_MAX);
                if (failure && owner_missing(reply)) {
                    char note[1024] = "", escaped[1200];
                    if (owner_start(port, note, sizeof(note)) == 1) {
                        failure = mg_client(port, request, reply, AGENT_REPLY_MAX);
                    } else {
                        if (escape_reply(escaped, sizeof(escaped), note)) {
                            snprintf(escaped, sizeof(escaped), "The adapter could not report why the owner did not start.");
                        }
                        snprintf(reply, AGENT_REPLY_MAX,
                                 "{\"ok\":false,\"image_path\":\"\","
                                 "\"fallback_text\":\"No native owner answered this action and the adapter could not start one. %s\","
                                 "\"repair_prompt\":\"Run 'megas serve' with the same --runtime folder and --port, or make the runtime folder writable and free localhost:%u, then retry. Existing VM disks and checkpoints are untouched.\"}",
                                 escaped, port);
                        failure = 1;
                    }
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
