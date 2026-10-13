/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void sleep_ms(unsigned ms)
{
    mgp_sleep(ms);
}

static void write_tree_ids(const char *path, long parent, long child)
{
    FILE *file = mgp_fopen(path, "wb");
    if (!file) {
        _exit(3);
    }
    fprintf(file, "%ld %ld\n", parent, child);
    fclose(file);
}

#ifdef _WIN32
static wchar_t *wide_utf8(const char *text)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    text, -1, NULL, 0);
    wchar_t *result;
    if (count <= 0) return NULL;
    result = malloc((size_t)count * sizeof(*result));
    if (!result) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             text, -1, result, count)) {
        free(result);
        return NULL;
    }
    return result;
}

static int helper_tree(const char *ready_path)
{
    wchar_t module[MAX_PATH + 1], command[2 * MAX_PATH + 128];
    wchar_t *ready = wide_utf8(ready_path);
    STARTUPINFOW startup;
    PROCESS_INFORMATION child;
    DWORD length = GetModuleFileNameW(NULL, module, MAX_PATH);
    if (!ready || !length || length >= MAX_PATH) {
        free(ready);
        return 2;
    }
    swprintf(command, sizeof(command) / sizeof(command[0]),
             L"\"%ls\" --tree-leaf \"%ls\"", module, ready);
    free(ready);
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &startup, &child)) {
        return 2;
    }
    write_tree_ids(ready_path, (long)GetCurrentProcessId(),
                   (long)child.dwProcessId);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    for (;;) Sleep(1000);
}

static int helper_leaf(void)
{
    for (;;) Sleep(1000);
    return 0;
}

static int process_exists(long pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, (DWORD)pid);
    DWORD result;
    int alive;
    if (!process) return 0;
    alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    if (alive && GetExitCodeProcess(process, &result) && result != STILL_ACTIVE) {
        alive = 0;
    }
    CloseHandle(process);
    return alive;
}
#else
static int helper_tree(const char *ready_path)
{
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        for (;;) pause();
    }
    write_tree_ids(ready_path, (long)getpid(), (long)child);
    for (;;) pause();
}

static int helper_leaf(void)
{
    for (;;) pause();
    return 0;
}

static int process_exists(long pid)
{
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}
#endif

static void prepare_temp_dir(char *out, size_t capacity)
{
#ifdef _WIN32
    wchar_t wide_temp[MAX_PATH + 1], wide_dir[MAX_PATH + 1];
    DWORD length = GetTempPathW(MAX_PATH, wide_temp);
    int bytes;
    if (!length || length >= MAX_PATH ||
        swprintf(wide_dir, MAX_PATH, L"%lsmegas-platform-%lu",
                 wide_temp, (unsigned long)GetCurrentProcessId()) < 0 ||
        !CreateDirectoryW(wide_dir, NULL)) {
        check(0, "create temporary test directory");
    }
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_dir, -1,
                                out, (int)capacity, NULL, NULL);
    check(bytes > 0, "convert temporary directory to UTF-8");
#else
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    check(snprintf(out, capacity, "%s/megas-platform-%ld-XXXXXX",
                   base, (long)getpid()) < (int)capacity,
          "format temporary directory");
    check(mkdtemp(out) != NULL, "create temporary test directory");
#endif
}

static void executable_path(const char *argv0, char *out, size_t capacity)
{
#ifdef _WIN32
    wchar_t wide[MAX_PATH + 1];
    DWORD length = GetModuleFileNameW(NULL, wide, MAX_PATH);
    int bytes;
    (void)argv0;
    check(length > 0 && length < MAX_PATH, "find test executable path");
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                out, (int)capacity, NULL, NULL);
    check(bytes > 0, "convert executable path to UTF-8");
#else
    char *resolved = realpath(argv0, NULL);
    check(resolved != NULL, "resolve test executable path");
    check(strlen(resolved) < capacity, "test executable path fits");
    strcpy(out, resolved);
    free(resolved);
#endif
}

static void verify_exclusive_create(const char *path)
{
    FILE *file;
    char contents[32] = {0};
    check(mgp_create_exclusive(path) == 0, "exclusive create of new file");
    file = mgp_fopen(path, "wb");
    check(file != NULL, "open newly-created file");
    fputs("preserve-existing", file);
    fclose(file);
    check(mgp_create_exclusive(path) < 0,
          "exclusive create must reject an existing file");
    file = mgp_fopen(path, "rb");
    check(file != NULL, "reopen existing file");
    check(fread(contents, 1, sizeof(contents) - 1, file) == strlen("preserve-existing"),
          "read existing file contents");
    fclose(file);
    check(!strcmp(contents, "preserve-existing"),
          "exclusive create must not truncate the existing file");
}

static void verify_argv_is_literal(const char *exe, const char *log)
{
    const char *quoted = "spaced argument with \\\"quotes\\\" and trailing slash\\";
    char expected[2048], error[512] = {0}, output[2048] = {0};
#ifdef _WIN32
    const char *injection = "literal&echo injected";
#else
    const char *injection = "literal;echo injected";
#endif
    char *args[5];
    MgpProcess process;
    FILE *file;
    size_t bytes;
    unsigned i;
    check(snprintf(expected, sizeof(expected), "%s|%s", quoted, injection) <
          (int)sizeof(expected), "format expected argv output");
    args[0] = (char *)exe;
    args[1] = "--argv-probe";
    args[2] = (char *)quoted;
    args[3] = (char *)injection;
    args[4] = NULL;
    memset(&process, 0, sizeof(process));
    check(mgp_spawn(&process, args, log, error, sizeof(error)) == 0,
          "spawn argv probe using an argument vector");
    for (i = 0; i < 500 && mgp_alive(&process); i++) sleep_ms(10);
    check(!mgp_alive(&process), "argv probe exits");
    mgp_reap(&process);
    file = mgp_fopen(log, "rb");
    check(file != NULL, "read argv probe log");
    bytes = fread(output, 1, sizeof(output) - 1, file);
    fclose(file);
    check(bytes == strlen(expected) + 1 && output[bytes - 1] == '\n',
          "argv probe writes one line with both literal arguments");
    output[bytes - 1] = 0;
    check(!strcmp(output, expected),
          "argv quoting preserves spaces, quotes, backslashes, and shell metacharacters");
}

static void verify_owned_tree_cleanup(const char *exe, const char *ready,
                                      const char *log)
{
    char *args[4], error[512] = {0};
    MgpProcess process;
    FILE *file;
    long parent, child;
    unsigned i;
    args[0] = (char *)exe;
    args[1] = "--tree-helper";
    args[2] = (char *)ready;
    args[3] = NULL;
    memset(&process, 0, sizeof(process));
    check(mgp_spawn(&process, args, log, error, sizeof(error)) == 0,
          "spawn owned process-tree helper");
    for (i = 0; i < 500 && mgp_file_size(ready) == 0; i++) sleep_ms(10);
    check(mgp_file_size(ready) > 0, "process-tree helper reports both PIDs");
    file = mgp_fopen(ready, "rb");
    check(file != NULL && fscanf(file, "%ld %ld", &parent, &child) == 2,
          "read process-tree helper PIDs");
    fclose(file);
    check(process_exists(parent) && process_exists(child),
          "owned process and its descendant are both live before stop");
    mgp_terminate(&process);
    for (i = 0; i < 500 && mgp_alive(&process); i++) sleep_ms(10);
    check(!mgp_alive(&process), "terminating owner reaps the complete process tree");
    mgp_reap(&process);
    for (i = 0; i < 500 &&
         (process_exists(parent) || process_exists(child)); i++) sleep_ms(10);
    check(!process_exists(parent) && !process_exists(child),
          "owned descendant is no longer running after cleanup");
}

int main(int argc, char **argv)
{
    char directory[1024], exe[2048], exclusive[1200], log[1200];
    char ready[1200];
    if (argc == 4 && !strcmp(argv[1], "--argv-probe")) {
        printf("%s|%s\n", argv[2], argv[3]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--tree-helper")) {
        return helper_tree(argv[2]);
    }
    if (argc == 3 && !strcmp(argv[1], "--tree-leaf")) {
        return helper_leaf();
    }
    check(mgp_init() == 0, "initialize process support");
    prepare_temp_dir(directory, sizeof(directory));
    executable_path(argv[0], exe, sizeof(exe));
    check(snprintf(exclusive, sizeof(exclusive), "%s/exclusive", directory) < (int)sizeof(exclusive),
          "format exclusive path");
    check(snprintf(log, sizeof(log), "%s/argv.log", directory) < (int)sizeof(log),
          "format argv log path");
    check(snprintf(ready, sizeof(ready), "%s/tree.ids", directory) < (int)sizeof(ready),
          "format process-tree path");

    verify_exclusive_create(exclusive);
    verify_argv_is_literal(exe, log);
    verify_owned_tree_cleanup(exe, ready, log);

    mgp_remove(exclusive);
    mgp_remove(log);
    mgp_remove(ready);
#ifdef _WIN32
    check(RemoveDirectoryA(directory) != 0, "remove temporary test directory");
#else
    check(rmdir(directory) == 0, "remove temporary test directory");
#endif
    puts("PASS: exclusive create, shell-free argv, owned process-tree cleanup");
    return 0;
}
