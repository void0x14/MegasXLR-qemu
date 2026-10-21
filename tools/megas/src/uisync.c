/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pure state rules for the native GUI. Kept free of engine, display and file
 * access so the rules that decide what the operator sees can be tested
 * without a running VM. */
#include "uisync.h"
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
    }
    if (!state->iso[0] && source->iso[0]) {
        size_t n = strlen(source->iso);
        if (n >= sizeof(state->iso)) n = sizeof(state->iso) - 1;
        memcpy(state->iso, source->iso, n);
        state->iso[n] = 0;
        changed = 1;
    }
    /* Memory and the disk controller belong to the VM. They are adopted from
     * the engine unless the operator explicitly changed them. */
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
