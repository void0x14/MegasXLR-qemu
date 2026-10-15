/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEGAS_H
#define MEGAS_H
#include <stdint.h>
#include <stddef.h>
#include "rfb.h"
typedef struct MgEngine MgEngine;
typedef struct {
    char runtime_dir[1024];
    char qemu[1024];
    char disk[1024];
    char iso[1024];
    char seed_iso[1024];
    unsigned ram_mb, width, height;
    unsigned qmp_port, vnc_port, control_port;
    unsigned qga_port;
    unsigned android_port, video_port, stream_port, cpus, cpu_percent, max_threads;
    char accel[16], cgroup_parent[1024], rendernode[64];
    int android;
    int passwordless;
} MgConfig;
typedef struct {
    char phase[64], feedback[512], repair_prompt[8192];
    uint64_t actions, failed, normalized, events, frame_generation;
    unsigned width, height;
    int connected, running, owned_pid;
    double last_restore_ms;
    unsigned qemu_threads;
    int android_ready, video_ready;
    uint64_t qemu_memory_bytes;
    char image_path[1024], journal_path[1024];
} MgStatus;
void mg_config_defaults(MgConfig *);
MgEngine *mg_engine_new(const MgConfig *);
/* All entry points run on the owner thread; tick services live RFB and IPC. */
int mg_engine_tick(MgEngine *);
int mg_engine_submit(MgEngine *, const char *request, char *response, size_t capacity);
const MgStatus *mg_engine_status(MgEngine *);
const MgConfig *mg_engine_config(MgEngine *);
size_t mg_engine_checkpoints(MgEngine *, char names[][64], size_t maximum);
MgRfb *mg_engine_display(MgEngine *);
void mg_engine_free(MgEngine *);
int mg_client(unsigned port, const char *request, char *response, size_t capacity);
int mg_gui_run(MgEngine *);
int mg_interrupted(void);
int mg_agent_stdio(unsigned port, int vision);
#endif
