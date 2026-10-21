/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pure state rules for the native GUI. Kept free of engine, display and file
 * access so the rules that decide what the operator sees can be tested
 * without a running VM. */
#include "uisync.h"
#include <stdio.h>
#include <string.h>

static const char *ui_known_bus(const char *bus)
{
    if (!bus || !bus[0]) return NULL;
    if (!strcmp(bus, "ide")) return bus;
    if (!strcmp(bus, "virtio")) return bus;
    if (!strcmp(bus, "scsi")) return bus;
    if (!strcmp(bus, "nvme")) return bus;
    return NULL;
}

/* The needle is the key in quotes followed by its colon, so the value starts
 * one character past the colon: one quote, the key, one quote, the colon. */
static const char *ui_field_start(const char *text, const char *key)
{
    char needle[64];
    if (!text || !key) return NULL;
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    return strstr(text, needle);
}

int mg_json_has(const char *text, const char *key)
{
    return ui_field_start(text, key) != NULL;
}

int mg_json_field_int(const char *text, const char *key, int fallback)
{
    const char *field = ui_field_start(text, key);
    int value = 0, digits = 0;
    if (!field) return fallback;
    field += strlen(key) + 3;
    while (*field == ' ') ++field;
    while (*field >= '0' && *field <= '9' && value < 100000000) {
        value = value * 10 + (*field - '0'); ++field; digits = 1;
    }
    return digits ? value : fallback;
}

int mg_json_field_bool(const char *text, const char *key, int fallback)
{
    const char *field = ui_field_start(text, key);
    if (!field) return fallback;
    field += strlen(key) + 3;
    return !strncmp(field, "true", 4);
}

int mg_json_field_text(const char *text, const char *key, char *out, size_t capacity)
{
    const char *field = ui_field_start(text, key);
    size_t used = 0;
    if (!field || !out || !capacity) return 0;
    field += strlen(key) + 3;
    if (*field != '"') return 0;
    ++field;
    while (*field && *field != '"' && used + 1 < capacity) {
        if (*field == '\\' && field[1]) ++field;
        out[used++] = *field++;
    }
    out[used] = 0;
    return 1;
}

int mg_ui_sync(MgUiSyncState *state, const MgUiSyncSource *source)
{
    int changed = 0;
    const char *bus;
    if (!state || !source) return 0;
    /* The engine owns the running VM's identity. The form adopts the disk it
     * reports, because a VM started outside the GUI would otherwise leave
     * the form showing the previous disk and send a second, wrong start.
     * A disk the operator chose themselves is never replaced. */
    if (!state->disk_dirty && source->disk[0] && strcmp(state->disk, source->disk)) {
        size_t n = strlen(source->disk);
        if (n >= sizeof(state->disk)) n = sizeof(state->disk) - 1;
        memcpy(state->disk, source->disk, n);
        state->disk[n] = 0;
        changed = 1;
        /* The engine filled the field, which is not the operator choosing it.
         * Clearing the mark here keeps that knowledge in one place, so the
         * caller cannot take the engine's write for an operator's choice. */
        state->disk_dirty = 0;
    }
    if (!state->iso[0] && source->iso[0]) {
        size_t n = strlen(source->iso);
        if (n >= sizeof(state->iso)) n = sizeof(state->iso) - 1;
        memcpy(state->iso, source->iso, n);
        state->iso[n] = 0;
        changed = 1;
    }
    /* Memory and the controller belong to a disk. The engine reports the
     * ones of the disk it runs, which say nothing about a different disk the
     * form names, so they are offered only for the disk the form shows.
     * Reaching here with a matching disk means either the form already named
     * it or the line above just adopted it. */
    if (!source->disk[0] || strcmp(state->disk, source->disk)) return changed;
    if (!state->ram_dirty && source->ram_mb >= 64 && source->ram_mb <= 32768 &&
        (unsigned)state->ram != source->ram_mb) {
        state->ram = (int)source->ram_mb;
        changed = 1;
    }
    bus = ui_known_bus(source->bus);
    if (!state->bus_dirty && bus && strcmp(state->bus, bus)) {
        size_t n = strlen(bus);
        if (n >= sizeof(state->bus)) n = sizeof(state->bus) - 1;
        memcpy(state->bus, bus, n);
        state->bus[n] = 0;
        changed = 1;
    }
    return changed;
}

int mg_ui_disk_touched(MgUiSyncState *state, const char *before, const char *after)
{
    if (!state || !before || !after || !strcmp(before, after)) return 0;
    state->disk_dirty = 1;
    return 1;
}

void mg_ui_text_pair(char *field, size_t field_capacity,
                     char *shadow, size_t shadow_capacity,
                     const char *value)
{
    char copy[1024];
    if (!field || !field_capacity || !shadow || !shadow_capacity || !value) return;
    /* The value may be the field itself, so it is copied before anything is
     * written. */
    snprintf(copy, sizeof(copy), "%s", value);
    snprintf(field, field_capacity, "%s", copy);
    /* The editor holds what the field holds, including any truncation, so the
     * two can never hold different text. */
    snprintf(shadow, shadow_capacity, "%s", field);
}

int mg_ui_start_allowed(const char *disk, const char *iso)
{
    if (disk && disk[0]) return 1;
    if (iso && iso[0]) return 1;
    return 0;
}

int mg_ui_resize_enabled(int connected, int running)
{
    return connected && running;
}
