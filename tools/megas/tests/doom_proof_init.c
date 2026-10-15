#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void mount_if_needed(const char *source, const char *target,
                            const char *type)
{
    unsigned long flags = MS_NOSUID;

    /* devtmpfs device nodes must remain usable by the framebuffer backend. */
    if (strcmp(type, "devtmpfs") != 0)
        flags |= MS_NODEV;
    if (mount(source, target, type, flags, NULL) != 0)
        perror(target);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/run", 0755);
    mount_if_needed("proc", "/proc", "proc");
    mount_if_needed("sysfs", "/sys", "sysfs");
    mount_if_needed("devtmpfs", "/dev", "devtmpfs");
    mkdir("/dev/dri", 0755);

    puts("DOOMGENERIC_LINUXVT_INIT_START");
    pid_t child = fork();
    if (child == 0) {
        execl("/usr/sbin/modprobe", "modprobe", "-v", "virtio_gpu",
              (char *)NULL);
        perror("modprobe");
        _exit(127);
    }
    if (child > 0) {
        int status = 0;
        if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            puts("DOOMGENERIC_LINUXVT_MODPROBE_FAILED");
            for (;;)
                pause();
        }
    } else {
        perror("fork");
        for (;;)
            pause();
    }

    sleep(2);
    pid_t lschild = fork();
    if (lschild == 0) {
        execl("/bin/busybox", "busybox", "ls", "-l", "/dev/fb0",
              "/dev/dri", "/dev/input", (char *)NULL);
        _exit(127);
    }
    if (lschild > 0)
        waitpid(lschild, NULL, 0);

    setenv("HOME", "/root", 1);
    puts("DOOMGENERIC_LINUXVT_EXEC");
    execl("/usr/local/bin/doomgeneric", "doomgeneric", "-iwad",
          "/usr/share/games/doom/freedoom2.wad", "-nosound", "-warp", "1",
          (char *)NULL);
    perror("doomgeneric");
    for (;;)
        pause();
}
