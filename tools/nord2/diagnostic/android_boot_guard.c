// SPDX-License-Identifier: MIT
/* Diagnostic PID 1 launcher. The installed Android init remains PID 1.
 * Its child follows PID 1 across switch_root through an open proc directory,
 * arms recovery, and saves the kernel ring in the backed-up expdb partition.
 * Nothing here interprets, formats or unlocks an Android filesystem.
 */
#include "nolibc.h"
#include <linux/fs.h>
#include <linux/reboot.h>
#include <linux/time.h>

#define LOG_LIMIT (16UL * 1024 * 1024)
#define EXPDB_BYTES (40UL * 1024 * 1024)
#define MISC_BYTES (512UL * 1024)
static char pending[LOG_LIMIT];
static unsigned long used, persisted, kmsg_overruns, dropped_bytes;
static int sink = -1, kmsg = -1, misc = -1;
static void append(char *to, const char *s) { strcpy(to + strlen(to), s); }
static char *contains(char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++) if (!strncmp(s, needle, n)) return s;
    return NULL;
}
static unsigned long seconds(void)
{
    struct timespec t = {0};
    if (my_syscall2(__NR_clock_gettime, 7, &t) < 0) return 0;
    return t.tv_sec;
}
static int at(int fd, const char *name, int flags)
{
    return __sysret(my_syscall4(__NR_openat, fd, name, flags, 0));
}
static int write_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}
static void record(const char *s, size_t n)
{
    if (n > sizeof(pending) - used) {
        dropped_bytes += n - (sizeof(pending) - used);
        n = sizeof(pending) - used;
    }
    memcpy(pending + used, s, n); used += n;
}
static void note(const char *s) { record(s, strlen(s)); record("\n", 1); }
static int flush_log(void)
{
    char header[4096] = {0};
    if (sink < 0) return -1;
    if (lseek(sink, 4096 + persisted, SEEK_SET) < 0 ||
        write_all(sink, pending + persisted, used - persisted) || fsync(sink)) return -1;
    memcpy(header, "NORD2-ANDROID-BOOT-V1", 21);
    memcpy(header + 32, &used, sizeof(used));
    memcpy(header + 40, &kmsg_overruns, sizeof(kmsg_overruns));
    memcpy(header + 48, &dropped_bytes, sizeof(dropped_bytes));
    if (lseek(sink, 0, SEEK_SET) != 0 || write_all(sink, header, sizeof(header)) || fsync(sink)) return -1;
    persisted = used;
    return 0;
}
static int arm(int fd)
{
    char old[32], command[32] = "boot-recovery";
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, old, sizeof(old)) != sizeof(old)) return -1;
    if (!memcmp(old, command, sizeof(old))) return 0;
    /* Never destroy a different command or recovery argument payload. */
    for (unsigned int i = 0; i < sizeof(old); i++) if (old[i]) return -1;
    if (lseek(fd, 0, SEEK_SET) != 0 || write_all(fd, command, sizeof(command)) || fsync(fd)) return -1;
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, old, sizeof(old)) != sizeof(old)) return -1;
    return memcmp(old, command, sizeof(old)) ? -1 : 0;
}
static int block(const char *sysname, const char *partname, const char *path, unsigned long bytes)
{
    char filename[128], text[4096] = {0};
    strcpy(filename, "/sys/class/block/"); append(filename, sysname); append(filename, "/uevent");
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
    if (n <= 0) return -1;
    strcpy(filename, "\nPARTNAME="); append(filename, partname); append(filename, "\n");
    if (!contains(text, filename)) return -1;
    char *ma = contains(text, "MAJOR="), *mi = contains(text, "MINOR=");
    if (!ma || !mi) return -1;
    unsigned int major = atol(ma + 6), minor = atol(mi + 6);
    if (major > 4095 || minor > 255) return -1;
    dev_t dev = makedev(major, minor);
    if (mknod(path, S_IFBLK | 0600, dev) && errno != EEXIST) return -1;
    struct stat st;
    if (stat(path, &st) || !S_ISBLK(st.st_mode) || st.st_rdev != dev) return -1;
    fd = open(path, O_RDWR | O_CLOEXEC);
    unsigned long actual = 0;
    if (fd < 0) return -1;
    if (ioctl(fd, BLKGETSIZE64, &actual) || actual != bytes) { close(fd); return -1; }
    return fd;
}
static void follow_root(int proc1)
{
    int root = at(proc1, "root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) return;
    if (!my_syscall1(__NR_fchdir, root)) { chroot("."); chdir("/"); }
    close(root);
}
static void request_recovery(void)
{
    pid_t pid = fork();
    if (!pid) {
        char *args[] = {"/system/bin/setprop", "sys.powerctl", "reboot,recovery", NULL};
        char *env[] = {"PATH=/system/bin:/vendor/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", NULL};
        execve(args[0], args, env); exit(127);
    }
}
static void snapshot_file(const char *path)
{
    char buf[8192];
    note(path);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { note("nord2-snapshot: unavailable"); return; }
    for (int i = 0; i < 32; i++) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        record(buf, n);
    }
    close(fd); note("nord2-snapshot: end");
}
static int awake_write(const char *path)
{
    const char *name = "nord2-android-diagnostic";
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ret = write_all(fd, name, strlen(name));
    close(fd); return ret;
}
static int awake_held(void)
{
    char names[8192] = {0};
    int fd = open("/sys/power/wake_lock", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, names, sizeof(names) - 1); close(fd);
    return n > 0 && contains(names, "nord2-android-diagnostic");
}
static void watchdog(int proc1)
{
    const unsigned long start = seconds();
    unsigned long last = 0, last_snapshot = 0;
    int requested = 0, armed = 0, reprobed = 0, metadata_log = 0, final_root = 0;
    int awake = 0, debug_mounted = 0;
    prctl(PR_SET_NAME, (unsigned long)"nord2-bootguard", 0, 0, 0);
    note("nord2-bootguard: started; Android timeout 240s, hard return 300s");
    for (;;) {
        unsigned long now = seconds();
        if (!final_root) {
            follow_root(proc1);
            struct stat init;
            if (!stat("/system/bin/init", &init)) final_root = 1;
        }
        if (!awake && !awake_write("/sys/power/wake_lock") && awake_held()) {
            awake = 1; note("nord2-bootguard: diagnostic suspend blocker verified");
        }
        if (final_root && !debug_mounted &&
            (!mount("debugfs", "/sys/kernel/debug", "debugfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) || errno == EBUSY)) {
            debug_mounted = 1; note("nord2-bootguard: debugfs available for display snapshots");
        }
        if (misc < 0) misc = block("sdc2", "misc", "/dev/nord2-misc", MISC_BYTES);
        if (misc >= 0 && !arm(misc) && !armed) { armed = 1; note("nord2-bootguard: recovery command verified"); }
        if (sink < 0) {
            sink = block("sdc8", "expdb", "/dev/nord2-expdb", EXPDB_BYTES);
            if (sink >= 0) note("nord2-bootguard: backed-up expdb log opened");
        }
        /* Android's crash collector also uses expdb. Move the complete RAM
         * log to a private regular file as soon as metadata is mounted. */
        struct stat metadata, root;
        if (!metadata_log && !stat("/metadata/vold", &metadata) &&
            !stat("/", &root) && metadata.st_dev != root.st_dev) {
            int fd = open("/metadata/nord2-android-v5-kernel.bin",
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0) {
                if (sink >= 0) close(sink);
                sink = fd; persisted = 0; metadata_log = 1;
                note("nord2-bootguard: RAM log moved to metadata file; header records losses");
            }
        }
        if (kmsg < 0) kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (kmsg >= 0) {
            char buf[8192];
            for (int i = 0; i < 512; i++) {
                ssize_t n = read(kmsg, buf, sizeof(buf));
                if (n < 0 && errno == EPIPE) { kmsg_overruns++; continue; }
                if (n <= 0) break;
                record(buf, n);
            }
        }
        /* Reuse the verified deferred-DSI workaround, after module loading. */
        struct stat st;
        if (!reprobed && !stat("/sys/module/oplus20615_samsung_ams643ye05_1080p_dsi_cmd", &st)) {
            if (stat("/sys/bus/platform/devices/1400e000.dsi/driver", &st)) {
                int fd = open("/sys/bus/platform/drivers_probe", O_WRONLY | O_CLOEXEC);
                if (fd >= 0) { write(fd, "1400e000.dsi", strlen("1400e000.dsi")); close(fd); }
            }
            reprobed = 1;
        }
        if (now - start >= 40 && now - last_snapshot >= 40) {
            snapshot_file("/sys/class/power_supply/battery/uevent");
            snapshot_file("/sys/class/power_supply/usb/uevent");
            snapshot_file("/sys/class/leds/lcd-backlight/brightness");
            snapshot_file("/sys/class/leds/lcd-backlight/max_brightness");
            snapshot_file("/sys/class/leds/lcd-backlight/max_hw_brightness");
            snapshot_file("/sys/kernel/debug/dri/0/state");
            snapshot_file("/sys/kernel/debug/mtkfb");
            last_snapshot = now;
        }
        if (now != last) { flush_log(); last = now; }
        if (!requested && (now - start >= 240 || (!awake && now - start >= 30))) {
            note("nord2-bootguard: requesting normal Android recovery shutdown");
            flush_log(); request_recovery(); requested = 1;
        }
        if (now - start >= 300 || (!awake && now - start >= 45)) {
            note("nord2-bootguard: hard recovery return"); flush_log();
            my_syscall0(__NR_sync);
            my_syscall4(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                        LINUX_REBOOT_CMD_RESTART2, "recovery");
        }
        struct timespec delay = {.tv_nsec = 100000000};
        my_syscall2(__NR_nanosleep, &delay, 0);
    }
}
static int selftest(const char *dir)
{
    char path[512], zeros[512] = {0}, check[512];
    if (strlen(dir) > 450) return 1;
    strcpy(path, dir); append(path, "/bcb-test");
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || write_all(fd, zeros, sizeof(zeros)) || arm(fd) || arm(fd)) return 1;
    lseek(fd, 0, SEEK_SET);
    if (read(fd, check, sizeof(check)) != sizeof(check) || memcmp(check, "boot-recovery", 13)) return 1;
    for (unsigned int i = 13; i < sizeof(check); i++) if (check[i]) return 1;
    lseek(fd, 0, SEEK_SET); write(fd, "x", 1);
    if (!arm(fd)) return 1;
    lseek(fd, 0, SEEK_SET); read(fd, check, 1); if (*check != 'x') return 1;
    close(fd);
    strcpy(path, dir); append(path, "/log-test");
    sink = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    note("first"); if (flush_log()) return 1;
    note("second"); if (flush_log()) return 1;
    lseek(sink, 32, SEEK_SET); unsigned long length = 0;
    if (read(sink, &length, sizeof(length)) != sizeof(length) || length != 13) return 1;
    lseek(sink, 4096, SEEK_SET); if (read(sink, check, length) != length || memcmp(check, "first\nsecond\n", length)) return 1;
    close(sink); puts("BCB idempotency, unknown-command rejection, payload preservation and log framing passed");
    return 0;
}
int main(int argc, char **argv, char **envp)
{
    if (argc == 3 && !strcmp(argv[1], "--selftest")) return selftest(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--wake-test")) {
        int take = awake_write("/sys/power/wake_lock"), held = awake_held();
        int drop = awake_write("/sys/power/wake_unlock");
        int left = awake_held();
        printf("wake acquisition=%d held=%d release=%d remaining=%d\n", take, held, drop, left);
        return take || !held || drop || left;
    }
    if (argc == 2 && !strcmp(argv[1], "--inspect")) {
        int a = block("sdc2", "misc", "/dev/nord2-misc", MISC_BYTES);
        int b = block("sdc8", "expdb", "/dev/nord2-expdb", EXPDB_BYTES);
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        printf("misc identity/size=%s expdb identity/size=%s; no writes\n", a>=0?"pass":"fail", b>=0?"pass":"fail");
        return a<0 || b<0;
    }
    if (getpid() != 1) { puts("Diagnostic launcher must be PID 1"); return 1; }
    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) return 1;
    int proc1 = open("/proc/1", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (proc1 < 0) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) { watchdog(proc1); return 1; }
    close(proc1);
    char *args[] = {"/init", NULL};
    execve("/nord2-first-stage", args, envp);
    return 1;
}
