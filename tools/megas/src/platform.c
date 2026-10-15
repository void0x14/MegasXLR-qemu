/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <wchar.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <dirent.h>
#endif
#endif
#ifdef _WIN32
static wchar_t *utf16(const char *text)
{
    int length;
    wchar_t *wide;
    if (!text || (length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               text, -1, NULL, 0)) <= 0) {
        return NULL;
    }
    wide = malloc((size_t)length * sizeof(*wide));
    if (!wide) { return NULL; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                            wide, length)) { free(wide); return NULL; }
    return wide;
}
#endif
#ifndef _WIN32
static void spawn_child_fail(int fd, int error)
{
    ssize_t written;
    do { written = write(fd, &error, sizeof(error)); }
    while (written < 0 && errno == EINTR);
    _exit(127);
}
#endif

FILE *mgp_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide = utf16(path), *wide_mode = utf16(mode);
    FILE *file = wide && wide_mode ? _wfopen(wide, wide_mode) : NULL;
    if (file) {
        HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
        if (handle != INVALID_HANDLE_VALUE) {
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        }
    }
    free(wide); free(wide_mode);
    return file;
#else
    FILE *file = fopen(path, mode);
    if (file) { fcntl(fileno(file), F_SETFD, FD_CLOEXEC); }
    return file;
#endif
}

int mgp_remove(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = utf16(path);
    int result = wide ? _wremove(wide) : -1;
    free(wide);
    return result;
#else
    return remove(path);
#endif
}

int mgp_env(const char *name, char *out, size_t capacity)
{
    if (!capacity) { return -1; }
    out[0] = 0;
#ifdef _WIN32
    wchar_t *wide_name = utf16(name), *value;
    DWORD count;
    int bytes;
    if (!wide_name) { return -1; }
    count = GetEnvironmentVariableW(wide_name, NULL, 0);
    if (!count) { free(wide_name); return -1; }
    value = malloc((size_t)count * sizeof(*value));
    if (!value) { free(wide_name); return -1; }
    if (!GetEnvironmentVariableW(wide_name, value, count)) {
        free(value); free(wide_name); return -1;
    }
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                NULL, 0, NULL, NULL);
    if (bytes <= 0 || (size_t)bytes > capacity ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                             out, (int)capacity, NULL, NULL)) {
        free(value); free(wide_name); return -1;
    }
    free(value); free(wide_name);
#else
    const char *value = getenv(name);
    if (!value || strlen(value) >= capacity) { return -1; }
    memcpy(out, value, strlen(value) + 1);
#endif
    return 0;
}

int mgp_create_exclusive(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = utf16(path);
    HANDLE handle = wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                                       0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) :
                           INVALID_HANDLE_VALUE;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(handle);
#else
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (close(fd)) return -1;
#endif
    return 0;
}

int mgp_replace(const char *source, const char *target)
{
#ifdef _WIN32
    wchar_t *from = utf16(source), *to = utf16(target);
    int result = from && to && MoveFileExW(from, to,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
    free(from); free(to);
    return result;
#else
    return rename(source, target);
#endif
}

int mgp_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    wchar_t *wide_name = utf16(name), *wide_value = utf16(value);
    int result = wide_name && wide_value &&
                 SetEnvironmentVariableW(wide_name, wide_value) ? 0 : -1;
    free(wide_name); free(wide_value);
    return result;
#else
    return setenv(name, value, 1);
#endif
}

int mgp_init(void)
{
#ifdef _WIN32
    static int initialized;
    WSADATA data;
    if (initialized) { return 0; }
    if (WSAStartup(MAKEWORD(2, 2), &data)) { return -1; }
    initialized = 1;
    return 0;
#else
#ifdef __linux__
    /* Reap adopted harness children instead of leaving zombies with init. */
    static int initialized;
    if (!initialized) {
        if (prctl(PR_SET_CHILD_SUBREAPER, 1)) return -1;
        initialized = 1;
    }
#endif
    return 0;
#endif
}
int mgp_executable_dir(char *out, size_t capacity)
{
    if (!out || !capacity) return -1;
    out[0] = 0;
#ifdef _WIN32
    {
        wchar_t path[32768];
        DWORD length = GetModuleFileNameW(NULL, path, (DWORD)(sizeof(path) / sizeof(path[0])));
        int bytes;
        wchar_t *slash;
        if (!length || length >= sizeof(path) / sizeof(path[0])) return -1;
        slash = wcsrchr(path, L'\\');
        if (!slash) slash = wcsrchr(path, L'/');
        if (!slash) return -1;
        *slash = 0;
        bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                    NULL, 0, NULL, NULL);
        if (bytes <= 0 || (size_t)bytes > capacity ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                 out, (int)capacity, NULL, NULL)) return -1;
        return 0;
    }
#elif defined(__linux__)
    {
        char path[4096];
        ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        char *slash;
        if (length <= 0 || (size_t)length >= sizeof(path)) return -1;
        path[length] = 0;
        slash = strrchr(path, '/');
        if (!slash) return -1;
        if (slash == path) slash[1] = 0;
        else *slash = 0;
        if (strlen(path) >= capacity) return -1;
        memcpy(out, path, strlen(path) + 1);
        return 0;
    }
#else
    return -1;
#endif
}
uint64_t mgp_now(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (unsigned)ts.tv_nsec / 1000000;
#endif
}
void mgp_sleep(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
#endif
}
int mgp_mkdir(const char *path)
{
    char copy[1024];
    size_t n = strlen(path), i;
    if (!n || n >= sizeof(copy)) return -1;
    memcpy(copy, path, n + 1);
    for (i = 1; i <= n; i++) {
        char saved = copy[i];
        if (saved && saved != '/' && saved != '\\') continue;
#ifdef _WIN32
        if (i == 2 && copy[1] == ':') continue;
#endif
        copy[i] = 0;
#ifdef _WIN32
        wchar_t *wide = utf16(copy);
        int failed = !wide || (_wmkdir(wide) && errno != EEXIST);
        free(wide);
        if (failed) { return -1; }
#else
        if (mkdir(copy, 0700) && errno != EEXIST) return -1;
#endif
        copy[i] = saved;
    }
    return 0;
}
uint64_t mgp_file_size(const char *path)
{
#ifdef _WIN32
    struct _stat64 st;
    wchar_t *wide = utf16(path);
    uint64_t result = wide && !_wstat64(wide, &st) ? (uint64_t)st.st_size : 0;
    free(wide);
    return result;
#else
    struct stat st;
    return stat(path, &st) ? 0 : (uint64_t)st.st_size;
#endif
}
uint64_t mgp_disk_free(const char *path)
{
#ifdef _WIN32
    ULARGE_INTEGER free_bytes;
    wchar_t *wide = utf16(path);
    uint64_t result = wide && GetDiskFreeSpaceExW(wide, &free_bytes, NULL, NULL) ?
                      free_bytes.QuadPart : 0;
    free(wide);
    return result;
#else
    struct statvfs st;
    return statvfs(path, &st) ? 0 : (uint64_t)st.f_bavail * st.f_frsize;
#endif
}
int mgp_random(void *data, size_t length)
{
#ifdef _WIN32
    typedef BOOLEAN (WINAPI *RandomFn)(PVOID, ULONG);
    HMODULE module = LoadLibraryA("advapi32.dll");
    RandomFn fn = module ? (RandomFn)(void *)GetProcAddress(module, "SystemFunction036") : NULL;
    int ok = fn && fn(data, (ULONG)length);
    if (module) FreeLibrary(module);
    return ok ? 0 : -1;
#else
    unsigned char *p = data;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        p += n; length -= (size_t)n;
    }
    close(fd);
    return 0;
#endif
}
void mgp_close(MgpSocket socket)
{
    if (socket == MGP_BAD_SOCKET) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}
int mgp_nonblock(MgpSocket socket)
{
#ifdef _WIN32
    u_long on = 1;
    if (!SetHandleInformation((HANDLE)(UINT_PTR)socket,
                               HANDLE_FLAG_INHERIT, 0)) return -1;
    return ioctlsocket(socket, FIONBIO, &on) ? -1 : 0;
#else
    int flags = fcntl(socket, F_GETFL);
    fcntl(socket, F_SETFD, FD_CLOEXEC);
    return flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}
int mgp_ready(MgpSocket socket, int writing, unsigned timeout_ms)
{
    fd_set fds;
    struct timeval tv = {(long)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000};
    FD_ZERO(&fds); FD_SET(socket, &fds);
    return select((int)socket + 1, writing ? NULL : &fds, writing ? &fds : NULL, NULL, &tv);
}
MgpSocket mgp_connect(unsigned port, unsigned timeout_ms)
{
    struct sockaddr_in addr;
    MgpSocket socket_fd;
    int err = 0;
#ifdef _WIN32
    int len = sizeof(err);
#else
    socklen_t len = sizeof(err);
#endif
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == MGP_BAD_SOCKET) return socket_fd;
    if (mgp_nonblock(socket_fd)) { mgp_close(socket_fd); return MGP_BAD_SOCKET; }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr))) {
        if (mgp_ready(socket_fd, 1, timeout_ms) <= 0 ||
            getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) || err) {
            mgp_close(socket_fd); return MGP_BAD_SOCKET;
        }
    }
    return socket_fd;
}
MgpSocket mgp_listen(unsigned port)
{
    struct sockaddr_in addr;
    MgpSocket s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == MGP_BAD_SOCKET) return s;
    mgp_nonblock(s);
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) || listen(s, 8)) {
        mgp_close(s); return MGP_BAD_SOCKET;
    }
    return s;
}
int mgp_recv(MgpSocket socket, void *data, size_t length)
{
    int n = (int)recv(socket, data, (int)length, 0);
    if (n >= 0) return n;
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK ? -2 : -1;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
#endif
}
int mgp_send(MgpSocket socket, const void *data, size_t length)
{
#ifdef MSG_NOSIGNAL
    int n = (int)send(socket, data, length, MSG_NOSIGNAL);
#else
    int n = (int)send(socket, data, (int)length, 0);
#endif
    if (n >= 0) return n;
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK ? -2 : -1;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
#endif
}
int mgp_send_all(MgpSocket socket, const void *data, size_t length, unsigned timeout_ms)
{
    const char *p = data;
    uint64_t deadline = mgp_now() + timeout_ms;
    while (length) {
        int n = mgp_send(socket, p, length);
        if (n == -2) {
            if (mgp_now() >= deadline || mgp_ready(socket, 1, 10) < 0) return -1;
            continue;
        }
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}
#ifdef _WIN32
static int quote_arg(char *out, size_t cap, size_t *used, const char *arg)
{
    size_t slashes = 0;
#define PUT(c) do { if (*used + 1 >= cap) return -1; out[(*used)++] = (c); } while (0)
    PUT('"');
    while (*arg) {
        if (*arg == '\\') { slashes++; arg++; continue; }
        if (*arg == '"') {
            while (slashes) { PUT('\\'); PUT('\\'); slashes--; }
            PUT('\\'); PUT('"');
        } else {
            while (slashes) { PUT('\\'); slashes--; }
            PUT(*arg);
        }
        arg++;
    }
    while (slashes) { PUT('\\'); PUT('\\'); slashes--; }
    PUT('"'); out[*used] = 0;
#undef PUT
    return 0;
}
#endif
int mgp_spawn_bounded(MgpProcess *process, char *const argv[], const char *logpath,
                      uint64_t memory_bytes, unsigned cpu_seconds,
                      char *error, size_t capacity)
{
    process->exit_code = -1;
#ifdef _WIN32
    STARTUPINFOW startup;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    wchar_t *wide_log = utf16(logpath), *wide_command;
    HANDLE log = wide_log ? CreateFileW(wide_log, FILE_APPEND_DATA,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) :
                            INVALID_HANDLE_VALUE;
    char command[32768]; size_t used = 0, i;
    free(wide_log);
    if (log == INVALID_HANDLE_VALUE) goto fail;
    memset(&startup, 0, sizeof(startup)); startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log; startup.hStdError = log;
    for (i = 0; argv[i]; i++) {
        if (i) {
            if (used + 1 >= sizeof(command)) { CloseHandle(log); goto fail; }
            command[used++] = ' ';
        }
        if (quote_arg(command, sizeof(command), &used, argv[i])) { CloseHandle(log); goto fail; }
    }
    wide_command = utf16(command);
    if (!wide_command || !CreateProcessW(NULL, wide_command, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
                            NULL, NULL, &startup, &pi)) {
        free(wide_command); CloseHandle(log); goto fail;
    }
    free(wide_command);
    process->job = CreateJobObjectW(NULL, NULL);
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (cpu_seconds) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
        limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
            (LONGLONG)cpu_seconds * 10000000LL;
    }
    if (memory_bytes) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = (SIZE_T)memory_bytes;
    }
    if (!process->job ||
        !SetInformationJobObject(process->job, JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(process->job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(log);
        if (process->job) { CloseHandle(process->job); process->job = NULL; }
        goto fail;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        TerminateJobObject(process->job, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(log);
        CloseHandle(process->job); process->job = NULL;
        goto fail;
    }
    CloseHandle(log); CloseHandle(pi.hThread);
    process->handle = pi.hProcess; process->pid = pi.dwProcessId;
    return 0;
fail:
    snprintf(error, capacity, "Program could not start (Windows code %lu). Repair prompt contains the log path.", GetLastError());
    return -1;
#else
    int pipefd[2], child_error = 0, log;
    pid_t pid;
    process->pid = 0; process->group = 0;
    process->host_pid = 0; process->host_scan_ms = 0;
    if (pipe(pipefd)) goto fail;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC); fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    log = open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log < 0) { close(pipefd[0]); close(pipefd[1]); goto fail; }
    pid = fork();
    if (pid == 0) {
        pid_t owner = getppid();
        close(pipefd[0]); dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log);
#ifdef __linux__
        if (prctl(PR_SET_PDEATHSIG, SIGKILL)) spawn_child_fail(pipefd[1], errno);
        if (getppid() != owner) spawn_child_fail(pipefd[1], ECHILD);
        /* Tesseract's OpenMP workers need additional data/stack mappings.
         * RLIMIT_DATA limits those allocations, not resident RSS; keep this
         * OCR child single-threaded so libgomp can stay within 48 MiB while
         * the separate RSS watchdog remains authoritative. */
        if (memory_bytes && setenv("OMP_THREAD_LIMIT", "1", 1)) {
            spawn_child_fail(pipefd[1], errno);
        }
#else
        (void)owner;
#endif
        if (memory_bytes) {
#ifdef RLIMIT_DATA
            struct rlimit limit;
            limit.rlim_cur = limit.rlim_max = (rlim_t)memory_bytes;
            if (setrlimit(RLIMIT_DATA, &limit)) {
                spawn_child_fail(pipefd[1], errno);
            }
#else
            spawn_child_fail(pipefd[1], ENOTSUP);
#endif
        }
        if (cpu_seconds) {
            struct rlimit limit;
            limit.rlim_cur = (rlim_t)cpu_seconds;
            limit.rlim_max = (rlim_t)cpu_seconds + 1;
            if (setrlimit(RLIMIT_CPU, &limit)) {
                spawn_child_fail(pipefd[1], errno);
            }
        }
        if (setsid() < 0) {
            spawn_child_fail(pipefd[1], errno);
        }
        execvp(argv[0], argv); spawn_child_fail(pipefd[1], errno);
    }
    close(log); close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); goto fail; }
    while (read(pipefd[0], &child_error, sizeof(child_error)) < 0 && errno == EINTR) { }
    close(pipefd[0]);
    if (child_error) { waitpid(pid, NULL, 0); errno = child_error; goto fail; }
    process->pid = pid; process->group = pid;
    process->host_pid = 0; process->host_scan_ms = 0;
    return 0;
fail:
    snprintf(error, capacity, "Program could not start: %s. Repair prompt contains the log path.", strerror(errno));
    return -1;
#endif
}
int mgp_spawn(MgpProcess *process, char *const argv[], const char *logpath,
              char *error, size_t capacity)
{
    return mgp_spawn_bounded(process, argv, logpath, 0, 0, error, capacity);
}
int mgp_alive(MgpProcess *process)
{
#ifdef _WIN32
    DWORD result;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
    if (!process->handle) return 0;
    if (WaitForSingleObject(process->handle, 0) == WAIT_TIMEOUT) return 1;
    if (GetExitCodeProcess(process->handle, &result)) process->exit_code = (int)result;
    if (process->job && QueryInformationJobObject(process->job,
            JobObjectBasicAccountingInformation, &accounting,
            sizeof(accounting), NULL)) { return accounting.ActiveProcesses != 0; }
    return 0;
#else
    int status;
    pid_t ended;
    while (process->group && (ended = waitpid(-process->group, &status, WNOHANG)) > 0) {
        if (ended == process->pid) {
            process->pid = 0;
            process->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                                 128 + WTERMSIG(status);
        }
    }
    if (process->pid) return 1;
    if (process->group && (!kill(-process->group, 0) || errno == EPERM)) return 1;
    process->group = 0;
    return 0;
#endif
}
int mgp_pid(MgpProcess *process)
{
#ifdef _WIN32
    return (int)process->pid;
#else
    return (int)(process->pid ? process->pid : process->group);
#endif
}
uint64_t mgp_memory_bytes(MgpProcess *process)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (!process || !process->handle) return 0;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(process->handle, (PROCESS_MEMORY_COUNTERS *)&counters,
                              sizeof(counters))) return 0;
    return counters.WorkingSetSize > (SIZE_T)counters.PrivateUsage ?
           (uint64_t)counters.WorkingSetSize : (uint64_t)counters.PrivateUsage;
#else
#ifdef __linux__
    char path[64], line[256];
    FILE *file;
    unsigned long long kb = 0;
    if (!process || process->pid <= 0) return 0;
    if (process->host_pid <= 0) {
        uint64_t now = mgp_now();
        if (process->host_scan_ms && now - process->host_scan_ms < 500) return 0;
        process->host_scan_ms = now;
        {
            char self[64];
            ssize_t count = readlink("/proc/self", self, sizeof(self) - 1);
            pid_t parent = 0;
            if (count > 0) {
                char *end;
                self[count] = 0;
                parent = (pid_t)strtol(self, &end, 10);
                if (*end) parent = 0;
            }
            if (parent > 0) {
                DIR *proc = opendir("/proc");
                struct dirent *entry;
                if (proc) {
                    while ((entry = readdir(proc)) != NULL) {
                        char *end;
                        long candidate = strtol(entry->d_name, &end, 10);
                        long parent_pid = -1, namespace_pid = -1;
                        if (!entry->d_name[0] || *end || candidate <= 0 || candidate > INT_MAX) continue;
                        snprintf(path, sizeof(path), "/proc/%ld/status", candidate);
                        file = mgp_fopen(path, "r");
                        if (!file) continue;
                        while (fgets(line, sizeof(line), file)) {
                            if (!strncmp(line, "PPid:", 5)) {
                                (void)sscanf(line + 5, "%ld", &parent_pid);
                            } else if (!strncmp(line, "NSpid:", 6)) {
                                const char *p = line + 6;
                                char *number_end;
                                while (*p) {
                                    long number;
                                    while (*p == ' ' || *p == '\t') p++;
                                    if (*p == '\n' || !*p) break;
                                    number = strtol(p, &number_end, 10);
                                    if (number_end == p) break;
                                    namespace_pid = number;
                                    p = number_end;
                                }
                            }
                        }
                        fclose(file);
                        if (parent_pid == (long)parent &&
                            (namespace_pid == (long)process->pid ||
                             (namespace_pid < 0 && candidate == (long)process->pid))) {
                            process->host_pid = (pid_t)candidate;
                            break;
                        }
                    }
                    closedir(proc);
                }
            }
        }
        if (process->host_pid <= 0) return 0;
    }
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)process->host_pid);
    file = mgp_fopen(path, "r");
    if (!file) return 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "VmRSS: %llu kB", &kb) == 1) break;
    }
    fclose(file);
    return (uint64_t)kb * 1024ULL;
#else
    (void)process;
    return 0;
#endif
#endif
}
void mgp_terminate(MgpProcess *process)
{
#ifdef _WIN32
    if (process->job) TerminateJobObject(process->job, 1);
    else if (mgp_alive(process)) TerminateProcess(process->handle, 1);
#else
    if (mgp_alive(process) && process->group) kill(-process->group, SIGTERM);
#endif
}
void mgp_force_kill(MgpProcess *process)
{
#ifdef _WIN32
    if (process->job) TerminateJobObject(process->job, 1);
    else if (mgp_alive(process)) TerminateProcess(process->handle, 1);
#else
    if (mgp_alive(process) && process->group) kill(-process->group, SIGKILL);
#endif
}
void mgp_reap(MgpProcess *process)
{
#ifdef _WIN32
    if (process->job) { CloseHandle(process->job); process->job = NULL; }
    if (process->handle) { CloseHandle(process->handle); process->handle = NULL; process->pid = 0; }
#else
    if (process->pid) {
        if (waitpid(process->pid, NULL, WNOHANG) == process->pid) process->pid = 0;
    }
    if (!process->pid && process->group && kill(-process->group, 0) && errno == ESRCH) {
        process->group = 0;
    }
#endif
}
int mgp_owner_lock(const char *path, intptr_t *lock)
{
#ifdef _WIN32
    wchar_t *wide = utf16(path);
    HANDLE handle = wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                                       0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) :
                           INVALID_HANDLE_VALUE;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    *lock = (intptr_t)handle;
#else
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    *lock = fd;
#endif
    return 0;
}
void mgp_owner_unlock(intptr_t lock)
{
#ifdef _WIN32
    CloseHandle((HANDLE)lock);
#else
    close((int)lock);
#endif
}
