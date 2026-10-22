/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MEGAS_PLATFORM_H
#define MEGAS_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET MgpSocket;
typedef struct { HANDLE handle, job; DWORD pid; int exit_code; } MgpProcess;
#define MGP_BAD_SOCKET INVALID_SOCKET
#else
#include <sys/types.h>
typedef int MgpSocket;
typedef struct {
    pid_t pid, group, host_pid;
    uint64_t host_scan_ms;
    int exit_code;
} MgpProcess;
#define MGP_BAD_SOCKET (-1)
#endif
int mgp_init(void);
int mgp_executable_dir(char *out, size_t capacity);
int mgp_self_path(char *out, size_t capacity);
FILE *mgp_fopen(const char *path, const char *mode);
int mgp_remove(const char *path);
int mgp_create_exclusive(const char *path);
int mgp_replace(const char *source, const char *target);
int mgp_env(const char *name, char *out, size_t capacity);
int mgp_setenv(const char *name, const char *value);
uint64_t mgp_now(void);
void mgp_sleep(unsigned ms);
int mgp_mkdir(const char *path);
uint64_t mgp_file_size(const char *path);
uint64_t mgp_disk_free(const char *path);
int mgp_random(void *data, size_t length);
void mgp_close(MgpSocket socket);
int mgp_nonblock(MgpSocket socket);
MgpSocket mgp_connect(unsigned port, unsigned timeout_ms);
MgpSocket mgp_listen(unsigned port);
int mgp_ready(MgpSocket socket, int writing, unsigned timeout_ms);
int mgp_recv(MgpSocket socket, void *data, size_t length);
int mgp_send(MgpSocket socket, const void *data, size_t length);
int mgp_send_all(MgpSocket socket, const void *data, size_t length, unsigned timeout_ms);
int mgp_spawn(MgpProcess *process, char *const argv[], const char *logpath, char *error, size_t capacity);
/* Detached spawn: the child leads its own session and is not killed with this
 * process, so a long-lived owner survives the adapter that started it. */
int mgp_spawn_detached(MgpProcess *process, char *const argv[], const char *logpath,
                       char *error, size_t capacity);
int mgp_spawn_bounded(MgpProcess *process, char *const argv[], const char *logpath,
                      uint64_t memory_bytes, unsigned cpu_seconds,
                      char *error, size_t capacity);
int mgp_alive(MgpProcess *process);
int mgp_pid(MgpProcess *process);
uint64_t mgp_memory_bytes(MgpProcess *process);
void mgp_terminate(MgpProcess *process);
void mgp_force_kill(MgpProcess *process);
void mgp_reap(MgpProcess *process);
int mgp_owner_lock(const char *path, intptr_t *lock);
/* Records the owning process id in the locked file, so a session names its
 * owner without a second registry. */
int mgp_owner_mark(intptr_t lock, unsigned long pid);
void mgp_owner_unlock(intptr_t lock);
const char *mgp_real_qemu(const char *requested, char *out, size_t cap);
#endif
