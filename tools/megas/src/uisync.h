/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pure state rules for the native GUI. No I/O, no engine handle: every rule
 * is a decision that the view layer applies, kept separate so it can be
 * tested without a display, an engine or a running VM. */
#ifndef MEGAS_UISYNC_H
#define MEGAS_UISYNC_H

#include <stddef.h>

/* The controller answers in JSON. These read one field out of an answer; a
 * field that is not there leaves the caller's fallback in place. */
int mg_json_has(const char *text, const char *key);
int mg_json_field_int(const char *text, const char *key, int fallback);
int mg_json_field_bool(const char *text, const char *key, int fallback);
int mg_json_field_text(const char *text, const char *key, char *out, size_t capacity);

/* The form fields the controller reconciles with the engine. */
typedef struct {
    char disk[1024];
    char iso[1024];
    char bus[16];
    int ram;
    int ram_dirty;
    int bus_dirty;
    int disk_dirty;
} MgUiSyncState;

/* What the engine reports about the VM it currently owns. */
typedef struct {
    char disk[1024];
    char iso[1024];
    char bus[16];
    unsigned ram_mb;
} MgUiSyncSource;

/* Reconcile the form with the engine's own record of the VM. A running VM's
 * identity belongs to the VM: the form adopts it instead of inventing one.
 * A field the user typed keeps its value; the engine never overwrites a
 * deliberate choice. Returns 1 when any field changed. */
int mg_ui_sync(MgUiSyncState *state, const MgUiSyncSource *source);

/* The disk controllers the form offers, in the order it offers them. */
extern const char *const mg_ui_bus_names[4];

/* Apply a recorded profile to a form field. A value the operator chose is
 * theirs: a profile never replaces it. Returns 1 when it was applied. */
int mg_ui_apply_profile(int *field, int dirty, int value, int minimum, int maximum);

/* Apply a recorded controller to the form. The controller belongs to the
 * disk, so it comes from the profile, unless the operator chose one
 * themselves. Returns 1 when it was applied. */
int mg_ui_apply_profile_bus(int *selected, int dirty, const char *value);

/* Whether the operator has changed a field themselves. The engine's own
 * write is not a change of theirs, so the two are told apart here rather
 * than by the caller: one rule, one place to keep correct. */
int mg_ui_disk_touched(MgUiSyncState *state, const char *before, const char *after);

/* The field and the editor's copy of it hold the same text. Writing one
 * without the other leaves the editor holding the previous text, which it
 * writes back over the field on the next frame and silently undoes the
 * change. Both copies are written together, through this. */
void mg_ui_text_pair(char *field, size_t field_capacity,
                     char *shadow, size_t shadow_capacity,
                     const char *value);

/* A VM without a disk and without an installer image has nothing to boot. */
int mg_ui_start_allowed(const char *disk, const char *iso);

/* The guest display can only be resized while the guest is live. */
int mg_ui_resize_enabled(int connected, int running);

#endif
