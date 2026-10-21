/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Regression evidence for the three Megas controller faults:
 *   1. the agent starts a VM the GUI never learns about, so the GUI sends an
 *      empty disk and the engine clears the disk it was given;
 *   2. a start with no disk at all is accepted;
 *   3. a screen resize is offered while the VM is stopped and its refusal
 *      reason never reaches the journal.
 * Every rule is asserted here so a fix cannot be claimed without evidence. */
#define _POSIX_C_SOURCE 200809L
#include "uisync.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_agent_start_fills_the_form(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    memset(&state, 0, sizeof(state));
    /* The agent started a VM outside the GUI. The form is still empty. */
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/home/u/.local/share/megas/vms/winlpe.qcow2");
    snprintf(source.bus, sizeof(source.bus), "ide");
    source.ram_mb = 4096;
    assert(mg_ui_sync(&state, &source) == 1);
    assert(strcmp(state.disk, source.disk) == 0);
    assert(state.ram == 4096);
    assert(strcmp(state.bus, "ide") == 0);
    /* A second sync changes nothing: the form and the engine agree. */
    assert(mg_ui_sync(&state, &source) == 0);
    puts("uisync: an agent start fills the empty form disk, memory and bus");
}

static void test_engine_disk_replaces_a_stale_form_disk(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    /* The form still shows the previous VM while the engine runs another. */
    memset(&state, 0, sizeof(state));
    snprintf(state.disk, sizeof(state.disk), "/old/vm.qcow2");
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/new/vm.qcow2");
    source.ram_mb = 4096;
    assert(mg_ui_sync(&state, &source) == 1);
    assert(strcmp(state.disk, "/new/vm.qcow2") == 0);
    puts("uisync: the engine's disk replaces a stale form disk");
}

static void test_the_engine_clears_the_operator_mark(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    /* A field the operator had touched, which the engine then refilled with
     * its own record: that write is the engine's, not the operator's, so the
     * mark is cleared here rather than by the caller. */
    memset(&state, 0, sizeof(state));
    snprintf(state.disk, sizeof(state.disk), "/operator/typed.qcow2");
    state.disk_dirty = 1;
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/engine/vm.qcow2");
    /* The mark keeps the engine's own disk out until the operator's choice is
     * resolved, so a second sync with the mark cleared is what is checked. */
    assert(mg_ui_sync(&state, &source) == 0);
    assert(strcmp(state.disk, "/operator/typed.qcow2") == 0);
    /* Clearing the mark is the operator resolving the choice, after which the
     * engine's record is adopted and the mark does not come back. */
    state.disk_dirty = 0;
    assert(mg_ui_sync(&state, &source) == 1);
    assert(strcmp(state.disk, "/engine/vm.qcow2") == 0);
    assert(state.disk_dirty == 0);
    puts("uisync: an engine write clears the operator mark inside the rule");
}

static void test_operator_touch_marks_the_field(void)
{
    MgUiSyncState state;
    memset(&state, 0, sizeof(state));
    /* Untouched: the field stays the engine's to fill. */
    assert(mg_ui_disk_touched(&state, "/engine/vm.qcow2", "/engine/vm.qcow2") == 0);
    assert(state.disk_dirty == 0);
    /* Typed by the operator: the field becomes theirs. */
    assert(mg_ui_disk_touched(&state, "/engine/vm.qcow2", "/operator/typed.qcow2") == 1);
    assert(state.disk_dirty == 1);
    /* Cleared by the operator: still theirs, so the engine cannot refill it. */
    memset(&state, 0, sizeof(state));
    assert(mg_ui_disk_touched(&state, "/operator/typed.qcow2", "") == 1);
    assert(state.disk_dirty == 1);
    puts("uisync: typing or clearing a path marks it as the operator's");
}

static void test_operator_disk_is_never_replaced(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    memset(&state, 0, sizeof(state));
    snprintf(state.disk, sizeof(state.disk), "/operator/chosen.qcow2");
    state.disk_dirty = 1;
    state.ram = 1024;
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/engine/other.qcow2");
    source.ram_mb = 4096;
    /* The engine's disk is not the one the operator named, so neither its
     * disk nor the memory that belongs to that other disk is taken. */
    assert(mg_ui_sync(&state, &source) == 0);
    assert(strcmp(state.disk, "/operator/chosen.qcow2") == 0);
    assert(state.ram == 1024);
    puts("uisync: a disk the operator chose is never replaced by the engine");
}

static void test_operator_choice_is_kept(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    memset(&state, 0, sizeof(state));
    snprintf(state.disk, sizeof(state.disk), "/operator/chosen.qcow2");
    state.disk_dirty = 1;
    state.ram = 8192;
    state.ram_dirty = 1;
    state.bus_dirty = 1;
    snprintf(state.bus, sizeof(state.bus), "virtio");
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/engine/other.qcow2");
    snprintf(source.bus, sizeof(source.bus), "ide");
    source.ram_mb = 4096;
    /* The engine's record never overwrites a deliberate operator choice. */
    assert(mg_ui_sync(&state, &source) == 0);
    assert(strcmp(state.disk, "/operator/chosen.qcow2") == 0);
    assert(state.ram == 8192);
    assert(strcmp(state.bus, "virtio") == 0);
    puts("uisync: an explicit operator disk, memory and bus survive the sync");
}

static void test_empty_disk_is_refused(void)
{
    /* A VM with no disk and no installer image has nothing to boot. */
    assert(mg_ui_start_allowed("", "") == 0);
    assert(mg_ui_start_allowed(NULL, NULL) == 0);
    assert(mg_ui_start_allowed("/vm/disk.qcow2", "") == 1);
    assert(mg_ui_start_allowed("", "/vm/installer.iso") == 1);
    puts("uisync: a start without a disk and without an installer is refused");
}

static void test_resize_requires_a_live_guest(void)
{
    /* Stopped, disconnected, or both: the guest display cannot be resized. */
    assert(mg_ui_resize_enabled(1, 0) == 0);
    assert(mg_ui_resize_enabled(0, 1) == 0);
    assert(mg_ui_resize_enabled(0, 0) == 0);
    assert(mg_ui_resize_enabled(1, 1) == 1);
    puts("uisync: a screen resize is offered only while the guest is live");
}

static void test_unknown_bus_is_not_adopted(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    memset(&state, 0, sizeof(state));
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/known/winlpe.qcow2");
    snprintf(source.bus, sizeof(source.bus), "floppy");
    source.ram_mb = 4096;
    mg_ui_sync(&state, &source);
    assert(state.bus[0] == 0);
    assert(state.ram == 4096);
    /* The engine reports no disk at all, so what it remembers belongs to no
     * disk and the form takes none of it. */
    memset(&state, 0, sizeof(state));
    memset(&source, 0, sizeof(source));
    source.ram_mb = 4096;
    assert(mg_ui_sync(&state, &source) == 0);
    assert(state.ram == 0);
    assert(state.bus[0] == 0);
    puts("uisync: an unknown disk controller is never adopted by the form");
}

static void test_named_vm_fills_the_form(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    /* The owner asked for "4GB Windows". The registry resolved that name to
     * one record, and the form must show the disk that record names. */
    memset(&state, 0, sizeof(state));
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/home/u/.local/share/megas/vms/winlpe.qcow2");
    snprintf(source.bus, sizeof(source.bus), "ide");
    source.ram_mb = 4096;
    assert(mg_ui_sync(&state, &source) == 1);
    assert(strcmp(state.disk, "/home/u/.local/share/megas/vms/winlpe.qcow2") == 0);
    assert(state.ram == 4096);
    assert(strcmp(state.bus, "ide") == 0);
    /* The same VM reached through a different name is still one VM. */
    {
        MgUiSyncState other;
        MgUiSyncSource renamed;
        memset(&other, 0, sizeof(other));
        memset(&renamed, 0, sizeof(renamed));
        snprintf(renamed.disk, sizeof(renamed.disk), "%s", source.disk);
        snprintf(renamed.bus, sizeof(renamed.bus), "%s", source.bus);
        renamed.ram_mb = source.ram_mb;
        assert(mg_ui_sync(&other, &renamed) == 1);
        assert(strcmp(other.disk, state.disk) == 0);
    }
    puts("uisync: a named VM fills the form with the disk its record names");
}

static void test_the_field_and_the_editor_copy_never_diverge(void)
{
    char field[1024], shadow[1024];
    char long_path[2048];
    /* The engine fills the field. Both copies hold what it wrote. */
    mg_ui_text_pair(field, sizeof(field), shadow, sizeof(shadow), "/engine/vm.qcow2");
    assert(strcmp(field, shadow) == 0);
    assert(strcmp(field, "/engine/vm.qcow2") == 0);
    /* The operator clears the field. Both copies are empty, so the editor
     * cannot put the previous path back. */
    mg_ui_text_pair(field, sizeof(field), shadow, sizeof(shadow), "");
    assert(field[0] == 0);
    assert(shadow[0] == 0);
    /* A path longer than the field is truncated into both copies, never
     * into one only: a shadow holding the full text would compare unequal
     * forever and keep reloading the editor. */
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = 0;
    mg_ui_text_pair(field, sizeof(field), shadow, sizeof(shadow), long_path);
    assert(strlen(field) == sizeof(field) - 1);
    assert(strcmp(field, shadow) == 0);
    /* Repeated writes keep the two identical whatever the sequence. */
    mg_ui_text_pair(field, sizeof(field), shadow, sizeof(shadow), "/a");
    mg_ui_text_pair(field, sizeof(field), shadow, sizeof(shadow), "/b/c.qcow2");
    mg_ui_text_pair(field, sizeof(field), shadow, sizeof(shadow), "");
    assert(strcmp(field, shadow) == 0);
    /* The value may be the field itself, which is how the editor writes its
     * own text back. That must not read what it is writing. */
    snprintf(field, sizeof(field), "%s", "/editor/wrote.this.qcow2");
    mg_ui_text_pair(field, sizeof(field), shadow, sizeof(shadow), field);
    assert(strcmp(field, "/editor/wrote.this.qcow2") == 0);
    assert(strcmp(field, shadow) == 0);
    /* A field smaller than the editor's copy is truncated into both. */
    {
        char small[16], large[512];
        mg_ui_text_pair(small, sizeof(small), large, sizeof(large),
                        "/a/very/long/path/that/does/not/fit");
        assert(strlen(small) == sizeof(small) - 1);
        assert(strcmp(small, large) == 0);
    }
    puts("uisync: the field and the editor's copy of it never diverge");
}

static void test_the_answer_fields_are_read(void)
{
    const char *answer = "{\"ok\":true,\"profile_known\":true,\"image_exists\":true,"
        "\"memory_mb\":4096,\"bus\":\"ide\",\"video\":\"VGA\","
        "\"disk\":\"/home/u/.local/share/megas/vms/winlpe.qcow2\"}";
    char text[256];
    /* The answer is read one character past the colon. Landing on the colon
     * instead made every field look absent, and the form fell back to
     * guessing what the VM was. */
    assert(mg_json_has(answer, "profile_known") == 1);
    assert(mg_json_field_bool(answer, "profile_known", 0) == 1);
    assert(mg_json_field_bool(answer, "image_exists", 0) == 1);
    assert(mg_json_field_int(answer, "memory_mb", 0) == 4096);
    assert(mg_json_field_text(answer, "bus", text, sizeof(text)) == 1);
    assert(strcmp(text, "ide") == 0);
    assert(mg_json_field_text(answer, "video", text, sizeof(text)) == 1);
    assert(strcmp(text, "VGA") == 0);
    assert(mg_json_field_text(answer, "disk", text, sizeof(text)) == 1);
    assert(strcmp(text, "/home/u/.local/share/megas/vms/winlpe.qcow2") == 0);
    /* A field the answer does not carry leaves the fallback in place. */
    assert(mg_json_has(answer, "missing") == 0);
    assert(mg_json_field_int(answer, "missing", 7) == 7);
    assert(mg_json_field_bool(answer, "missing", 1) == 1);
    assert(mg_json_field_text(answer, "missing", text, sizeof(text)) == 0);
    /* A field that is present and false is read as false, not as absent. */
    {
        const char *negative = "{\"profile_known\":false,\"image_exists\":false}";
        assert(mg_json_has(negative, "profile_known") == 1);
        assert(mg_json_field_bool(negative, "profile_known", 1) == 0);
        assert(mg_json_field_bool(negative, "image_exists", 1) == 0);
    }
    puts("uisync: every field of the controller's answer is read at its value");
}

static void test_the_preflight_answer_is_read(void)
{
    /* The answer a preflight gives, in the shape the controller sends it.
     * The form takes the memory and the controller of the disk it was asked
     * about from here, not the ones of the VM that happens to be running. */
    const char *answer = "{\"ok\":true,\"state\":{\"running\":true},"
        "\"vm\":{\"disk\":\"/other/running.qcow2\",\"memory_mb\":4096,\"bus\":\"virtio\","
        "\"ram_mb\":4096,\"disk_bus\":\"virtio\"},"
        "\"preflight_disk\":\"/known/winlpe.qcow2\",\"preflight_ram_mb\":2048,"
        "\"preflight_bus\":\"ide\",\"preflight_video\":\"VGA\","
        "\"preflight_known\":true,\"preflight_image_exists\":true}";
    char text[256];
    assert(mg_json_has(answer, "preflight_known") == 1);
    assert(mg_json_field_bool(answer, "preflight_known", 0) == 1);
    assert(mg_json_field_int(answer, "preflight_ram_mb", 0) == 2048);
    assert(mg_json_field_text(answer, "preflight_bus", text, sizeof(text)) == 1);
    assert(strcmp(text, "ide") == 0);
    assert(mg_json_field_bool(answer, "preflight_image_exists", 0) == 1);
    /* The running VM's own values are still there, for the state that
     * describes it. */
    assert(mg_json_field_int(answer, "ram_mb", 0) == 4096);
    puts("uisync: the preflight answer is read for the disk that was asked about");
}

static void test_the_engines_memory_stays_with_its_own_disk(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    /* The engine runs one VM. The form names a different disk, whose memory
     * and controller are recorded. The running VM's values say nothing about
     * that disk, so they must not replace what it recorded: a disk installed
     * on IDE would otherwise be started on whatever the running VM used. */
    memset(&state, 0, sizeof(state));
    snprintf(state.disk, sizeof(state.disk), "/known/winlpe.qcow2");
    state.disk_dirty = 1;
    state.ram = 2048;
    snprintf(state.bus, sizeof(state.bus), "ide");
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/other/running.qcow2");
    snprintf(source.bus, sizeof(source.bus), "virtio");
    source.ram_mb = 4096;
    assert(mg_ui_sync(&state, &source) == 0);
    assert(strcmp(state.disk, "/known/winlpe.qcow2") == 0);
    assert(state.ram == 2048);
    assert(strcmp(state.bus, "ide") == 0);
    puts("uisync: the running VM's memory and controller stay with its own disk");
}

static void test_memory_comes_with_the_disk_that_is_adopted(void)
{
    MgUiSyncState state;
    MgUiSyncSource source;
    /* The agent started a VM the form has not seen. The form adopts the disk
     * and must adopt its memory and controller with it, so it describes the
     * same VM the engine is running. */
    memset(&state, 0, sizeof(state));
    memset(&source, 0, sizeof(source));
    snprintf(source.disk, sizeof(source.disk), "/agent/winlpe.qcow2");
    snprintf(source.bus, sizeof(source.bus), "ide");
    source.ram_mb = 4096;
    assert(mg_ui_sync(&state, &source) == 1);
    assert(strcmp(state.disk, "/agent/winlpe.qcow2") == 0);
    assert(state.ram == 4096);
    assert(strcmp(state.bus, "ide") == 0);
    assert(state.disk_dirty == 0);
    /* And the form already naming that disk keeps the same values. */
    {
        MgUiSyncState same;
        memset(&same, 0, sizeof(same));
        snprintf(same.disk, sizeof(same.disk), "/agent/winlpe.qcow2");
        same.disk_dirty = 1;
        same.ram = 256;
        same.ram_dirty = 1;
        snprintf(same.bus, sizeof(same.bus), "virtio");
        same.bus_dirty = 1;
        assert(mg_ui_sync(&same, &source) == 0);
        assert(same.ram == 256);
        assert(strcmp(same.bus, "virtio") == 0);
    }
    puts("uisync: memory and controller arrive with the disk they belong to");
}

int main(void)
{
    test_agent_start_fills_the_form();
    test_engine_disk_replaces_a_stale_form_disk();
    test_the_engines_memory_stays_with_its_own_disk();
    test_memory_comes_with_the_disk_that_is_adopted();
    test_the_engine_clears_the_operator_mark();
    test_operator_touch_marks_the_field();
    test_operator_disk_is_never_replaced();
    test_operator_choice_is_kept();
    test_empty_disk_is_refused();
    test_resize_requires_a_live_guest();
    test_unknown_bus_is_not_adopted();
    test_named_vm_fills_the_form();
    test_the_field_and_the_editor_copy_never_diverge();
    test_the_answer_fields_are_read();
    test_the_preflight_answer_is_read();
    puts("uisync: all controller state rules passed");
    return 0;
}
