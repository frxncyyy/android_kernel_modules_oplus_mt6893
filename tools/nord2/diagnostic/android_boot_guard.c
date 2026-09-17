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
static char *decimal(char *out, unsigned long value);

/* The stock firmware trees are intentionally left alone.
 *
 * init mounts /odm (dm-5) and /vendor (dm-1) at about 9s, and the firmware loader reads
 * in init's mount namespace (kernel_read_file_from_path_initns,
 * firmware_loader/main.c:612), so everything under /odm/firmware - 893 AW8697/RTP
 * waveforms at the root plus the per-panel tp/ trees - becomes reachable without help.
 * That is what haptics needs, and an earlier build that embedded a handful of files in
 * the ramdisk could not cover it (~267 MB against a 32 MB boot image).
 *
 * Touch is the one driver that must NOT be allowed to consume these files, and an
 * earlier round masked /odm/firmware/tp here to achieve that.  That mask is no longer
 * needed: the FT3518 driver itself now refuses to flash the on-disk image
 * (ft3518_driver.c: fts_fw_update skips any image whose version byte is 0x7e), because
 * that image is the wrong one for this panel and reflashing with it leaves the
 * controller answering 0x00ef instead of 0x5452.  Fixing the driver is the right layer -
 * it holds whether or not the file is reachable and survives future changes to the
 * firmware search path - so this function is deliberately a no-op and the trees stay
 * mounted for every other subsystem.
 */
static int firmware_paths_ok(void)
{
    struct stat st;
    if (stat("/odm/firmware", &st)) {
        note("nord2-fw: /odm/firmware not present yet");
        return 0;
    }
    note("nord2-fw: /odm and /vendor are mounted by init; firmware left reachable "
         "(touch is guarded in the FT3518 driver, not by hiding the tree)");
    return 1;
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
/* A round enabled the vendor always-on display to see whether the port could
 * honour it.  It cannot: entering doze leaves the panel dark and no later wake
 * restores it, and because the preference lives in userdata the failure follows
 * the phone back to ColorOS.  The setting must therefore be cleared from a root
 * context; ColorOS denies the write to both adb and the vendor shell, while this
 * launcher is root.  The child writes its answer to a file because its stdout is
 * not this process's log. */
static void run_aod_script(const char *script)
{
    pid_t pid = fork();
    if (!pid) {
        char *args[] = {"/system/bin/sh", "-c", (char *)script, NULL};
        char *env[] = {"PATH=/system/bin:/vendor/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", NULL};
        execve(args[0], args, env); exit(127);
    }
}
static void run_aod_set(const char *value)
{
    char cmd[512];
    strcpy(cmd, "{ for k in Setting_AodEnable Setting_AodSwitchEnable Setting_AodEnableImmediate; do "
                 "settings put secure $k ");
    append(cmd, value);
    append(cmd, "; done; for k in Setting_AodEnable Setting_AodSwitchEnable; do echo -n \"$k=\"; "
                 "settings get secure $k; done; } > /data/local/tmp/nord2-settings.txt 2>&1");
    run_aod_script(cmd);
}
/* The round has to turn the vendor AOD on to exercise it, so the operator's own
 * choice is saved first and put back at the end; clearing it for good would
 * silently change their phone. */
static void run_aod_save_and_clear(void)
{
    run_aod_script("{ settings get secure Setting_AodEnable; settings get secure Setting_AodSwitchEnable; } "
                   "> /data/local/tmp/nord2-aod-orig.txt 2>&1; "
                   "{ for k in Setting_AodEnable Setting_AodSwitchEnable Setting_AodEnableImmediate; do "
                   "settings put secure $k 0; done; "
                   "for k in Setting_AodEnable Setting_AodSwitchEnable; do echo -n \"$k=\"; "
                   "settings get secure $k; done; } > /data/local/tmp/nord2-settings.txt 2>&1");
}
static void run_aod_restore(void)
{
    run_aod_script("{ read aod; read sw; settings put secure Setting_AodEnable ${aod:-0}; "
                   "settings put secure Setting_AodSwitchEnable ${sw:-0}; "
                   "for k in Setting_AodEnable Setting_AodSwitchEnable; do echo -n \"$k=\"; "
                   "settings get secure $k; done; } < /data/local/tmp/nord2-aod-orig.txt "
                   "> /data/local/tmp/nord2-settings.txt 2>&1");
}
static void show_aod_fix(void)
{
    char buf[512] = {0};
    int fd = open("/data/local/tmp/nord2-settings.txt", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { note("nord2-aod-fix: no result file"); return; }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) { note("nord2-aod-fix: empty result"); return; }
    buf[n] = 0;
    note("nord2-aod-fix: settings now");
    note(buf);
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

/* Collect the evidence that identifies a crashing ColorOS service.
 *
 * Run from the guard's own process so the shell inherits the guard's place in init's
 * mount namespace and can see /system, /vendor and /odm.  Output is captured through a
 * pipe rather than a file: the ramdisk is read-only and /data is not mounted during a
 * diagnostic boot, so there is nowhere to stage an intermediate file.  Each command's
 * output is folded into the log with a tag, so a later reading can tell which dump a
 * line came from.
 */
static void run_svc_dump(const char *tag)
{
    /* A crashing service shows up in init's own property state, in the tombstone
     * directory, and in the logd buffer for the crash itself.  Ask for all three. */
    static const char *cmds[] = {
        "/system/bin/getprop | /system/bin/grep -E 'init.svc.(fuelgauged|vpud|fps_hal|wifisar|mnld|vendor\\.)'",
        "/system/bin/ls -la /data/tombstones",
        "/system/bin/dmesg | /system/bin/grep -iE 'died|crash|fatal|signal|tombstone|avc: +denied' | /system/bin/tail -n 40",
        /* The fuel-gauge loader writes its own failure to /dev/kmsg as MTK_FG_FUEL, naming
         * exactly why it exited.  /vendor/bin/fuelgauged only dlopen()s
         * /vendor/lib/libfgauge_gm30.so and exits 0 when libfgauge_setup or its init fails
         * ("load 'libfgauge_setup' error", "init failed, return!"), so this line is the whole
         * diagnosis - and it does not match the crash keywords above, which is why the first
         * capture missed it. */
        "/system/bin/dmesg | /system/bin/grep -iE 'MTK_FG_FUEL|fgauge|fuelgauge|GM3' | /system/bin/tail -n 30",
        /* GPU: the kernel driver must bind and report the same GPU the userspace blob was
         * built for.  The port ships a prebuilt mali_kbase_mt6893_r49.ko (the source tree's
         * own driver is not built because CONFIG_MTK_GPU_VERSION is empty), so ask the
         * driver what it found and whether the Valhall workaround blob loaded. */
        "/system/bin/dmesg | /system/bin/grep -iE 'mali|kbase|gpu' | /system/bin/tail -n 40",
        "/system/bin/cat /sys/class/misc/mali0/device/gpuinfo 2>/dev/null",
        /* Codec: hardware encode/decode needs the vcodec/ION/SMI stack.  Stock has
         * VIDEO_MEDIATEK_VCODEC=y, MTK_ION=y and MTK_SMI_EXT=y; report whether the port's
         * kernel registered those devices at all, and what the codec HAL can see. */
        /* v119: the codec drivers load but nothing binds to them, so capture the probe
         * result directly.  The driver defers (-EPROBE_DEFER) when the VCU device or the
         * IOMMU domain is not ready, and a deferral leaves no error line at all - hence
         * grepping for the deferral text and the driver's own debug output, and listing
         * the bound devices so a successful probe is visible too. */
        "/system/bin/dmesg | /system/bin/grep -iE 'vcodec|vdec|venc|jpgenc|mtk-ion|ion_|smi-|smi_|mtk_iommu|VCU|probe defer|EPROBE' | /system/bin/tail -n 40",
        "for d in mtk-vcodec-dec mtk-vcodec-enc mtk-jpeg mtk_vcu mtk_iommu mtk-iommu mtk-iommu-v2; do echo \"$d: [$(/system/bin/ls /sys/bus/platform/drivers/$d/ 2>/dev/null | /system/bin/grep -vE '^(bind|unbind|uevent|module)$' | /usr/bin/tr '\\n' ' ')]\"; done",
        "/system/bin/cat /sys/kernel/debug/devices_deferred 2>/dev/null | /system/bin/head -20",
        "/system/bin/cat /proc/device-tree/vdec@16000000/status 2>/dev/null; echo; /system/bin/cat /proc/device-tree/venc@17000000/status 2>/dev/null; echo",
        "/system/bin/ls /dev/video* /dev/jpeg* /dev/vcu* 2>/dev/null",
        "/system/bin/ls /dev/ | /system/bin/grep -iE 'mali|dri|ion|mtk|vcodec|venc|vdec|apusys|mdla|vpu|m4u|smi'",
    };
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        int fds[2];
        if (pipe(fds)) continue;
        pid_t pid = fork();
        if (!pid) {
            close(fds[0]);
            dup2(fds[1], 1);
            close(fds[1]);
            char *args[] = {"/system/bin/sh", "-c", (char *)cmds[i], NULL};
            char *env[] = {"PATH=/system/bin:/vendor/bin", "ANDROID_ROOT=/system",
                           "ANDROID_DATA=/data", NULL};
            execve(args[0], args, env);
            my_syscall1(__NR_exit, 127);
        }
        close(fds[1]);
        char b[256];
        strcpy(b, "nord2-svc[");
        append(b, tag);
        append(b, "]: ");
        append(b, cmds[i]);
        note(b);
        char buf[1024];
        ssize_t n;
        int total = 0;
        while ((n = read(fds[0], buf, sizeof(buf) - 1)) > 0 && total < 6000) {
            buf[n] = 0;
            record(buf, (size_t)n);
            total += (int)n;
        }
        close(fds[0]);
        note("nord2-svc: end");
    }
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
static int display_debug_command(const char *command)
{
    int fd = open("/sys/kernel/debug/mtkfb", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ret = write_all(fd, command, strlen(command)); close(fd); return ret;
}
/* Power probe.  The diagnostic wake lock this launcher holds is exactly what
 * keeps every round awake, so suspend can only be observed once it is dropped
 * on purpose.  The probe is opt-in through a file in the ramdisk, read before
 * switch_root replaces it, so ordinary rounds keep the wake lock throughout.
 * CLOCK_BOOTTIME counts suspended time and CLOCK_MONOTONIC does not, so the
 * difference between the two clocks measures any suspend that happened while
 * this process was frozen. */
static char probe_cfg[512];
static unsigned long monotonic(void)
{
    struct timespec t = {0};
    if (my_syscall2(__NR_clock_gettime, 1, &t) < 0) return 0;
    return t.tv_sec;
}
static unsigned long value_of(const char *path)
{
    char buf[64] = {0};
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 ? (unsigned long)atol(buf) : 0;
}
static int value_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ret = write_all(fd, value, strlen(value));
    close(fd);
    return ret;
}
static char *decimal(char *out, unsigned long value)
{
    char digits[24];
    int n = 0, i = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) out[i++] = digits[--n];
    out[i] = 0;
    return out;
}
static unsigned long probe_value(const char *key)
{
    char needle[32];
    strcpy(needle, key); append(needle, "=");
    char *hit = contains(probe_cfg, needle);
    return hit ? (unsigned long)atol(hit + strlen(needle)) : 0;
}
/* Copy the whitespace-delimited token after "key=" into out. */
static int probe_text(const char *key, char *out, unsigned long size)
{
    char needle[32];
    unsigned long n = 0;
    strcpy(needle, key); append(needle, "=");
    char *hit = contains(probe_cfg, needle);
    if (!hit || !size) return -1;
    for (hit += strlen(needle); *hit && n < size - 1; hit++) {
        if (*hit == ' ' || *hit == '\n' || *hit == '\r' || *hit == '\t') break;
        out[n++] = *hit;
    }
    out[n] = 0;
    return n ? 0 : -1;
}
/* Arm the PMIC RTC so that a suspend with nothing else to wake it still ends,
 * and so that every pass through the suspend/resume loop has a fresh safety net. */
static void arm_probe_alarm(unsigned long seconds)
{
    char text[32];
    unsigned long base = value_of("/sys/class/rtc/rtc0/since_epoch");
    if (!base) { note("nord2-power-probe: no RTC device present"); return; }
    decimal(text, base + seconds);
    if (!value_write("/sys/class/rtc/rtc0/wakealarm", text)) {
        note("nord2-power-probe: RTC wake alarm armed");
        snapshot_file("/sys/class/rtc/rtc0/wakealarm");
    } else note("nord2-power-probe: RTC wake alarm write failed");
}
static void power_snapshot(void)
{    snapshot_file("/sys/power/state");
    snapshot_file("/sys/power/mem_sleep");
    snapshot_file("/sys/power/wake_lock");
    snapshot_file("/sys/power/suspend_stats/success");
    snapshot_file("/sys/power/suspend_stats/fail");
    snapshot_file("/sys/power/suspend_stats/last_failed_dev");
    snapshot_file("/sys/power/suspend_stats/last_failed_errno");
    snapshot_file("/sys/kernel/debug/wakeup_sources");
    snapshot_file("/sys/class/rtc/rtc0/name");
    snapshot_file("/sys/class/rtc/rtc0/since_epoch");
    snapshot_file("/sys/class/rtc/rtc0/wakealarm");
    snapshot_file("/sys/class/rtc/rtc0/hctosys");
}
static void watchdog(int proc1)
{
    const unsigned long start = seconds();
    unsigned long last = 0, last_snapshot = 0;
    unsigned long probe_at = probe_value("probe_at"), probe_alarm = probe_value("alarm_after");
    unsigned long probe_drop = probe_value("drop"), probe_boot = 0, probe_mono = 0;
    unsigned long probe_released = 0, probe_success = 0, probe_fail = 0;
    unsigned long probe_open = probe_value("hold_open"), probe_cycles = 0;
    unsigned long fix_aod = probe_value("fix_aod"), aod_test = probe_value("aod_test");
    int fix1 = 0, fix2 = 0, fix3 = 0, show1 = 0, show2 = 0, show3 = 0, aod_on = 0;
    char probe_mem[16] = {0};
    probe_text("mem_sleep", probe_mem, sizeof(probe_mem));
    int requested = 0, armed = 0, reprobed = 0, metadata_log = 0, final_root = 0;
    int fw_masked = 0, tries = 0;
    int svc_probe1 = 0, svc_probe2 = 0, svc_probe3 = 0;
    int awake = 0, debug_mounted = 0, display_logger = 0, probe_done = 0, probe_hold = 0;
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
        /* Note once that init's firmware mounts are up.  Nothing is masked: the FT3518
         * driver refuses the bad image itself, so /odm/firmware stays reachable for
         * haptics while touch is safe. */
        if (final_root && !fw_masked && tries < 12 && now - start >= 8) {
            tries++;
            if (firmware_paths_ok()) fw_masked = 1;
        }
        if (debug_mounted && !display_logger && now - start >= 18 &&
            !display_debug_command("logger:on")) {
            display_logger = 1;
            note("nord2-bootguard: display register logger enabled");
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
            int fd = open("/metadata/nord2-android-v11-kernel.bin",
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
        if (probe_at && !probe_done && now - start >= probe_at) {
            probe_done = 1;
            probe_boot = seconds(); probe_mono = monotonic();
            probe_success = value_of("/sys/power/suspend_stats/success");
            probe_fail = value_of("/sys/power/suspend_stats/fail");
            note("nord2-power-probe: begin");
            power_snapshot();
            if (probe_mem[0]) {
                if (!value_write("/sys/power/mem_sleep", probe_mem)) {
                    note("nord2-power-probe: memory sleep state selected");
                    snapshot_file("/sys/power/mem_sleep");
                } else note("nord2-power-probe: memory sleep state write failed");
            }
            if (probe_alarm) arm_probe_alarm(probe_alarm);
            if (probe_drop) {
                if (!value_write("/sys/power/wake_unlock", "nord2-android-diagnostic")) {
                    probe_hold = 1;
                    note("nord2-power-probe: diagnostic wake lock released");
                } else note("nord2-power-probe: wake lock release failed");
            }
            probe_released = now;
            flush_log();
        }
        /* While the lock is released the system can freeze this process, so each
         * pass here means at least one suspend and resume completed.  The window
         * stays open for probe_open seconds when asked, re-arming the alarm every
         * cycle, and always closes with the lock reacquired. */
        if (probe_hold) {
            unsigned long success = value_of("/sys/power/suspend_stats/success");
            unsigned long fail = value_of("/sys/power/suspend_stats/fail");
            unsigned long boot = seconds(), mono = monotonic();
            char text[160];
            if (success != probe_success || fail != probe_fail) {
                probe_success = success; probe_fail = fail; probe_cycles++;
                strcpy(text, "nord2-power-probe: cycle "); decimal(text + strlen(text), probe_cycles);
                append(text, " success="); decimal(text + strlen(text), success);
                append(text, " fail="); decimal(text + strlen(text), fail);
                append(text, " boot_delta="); decimal(text + strlen(text), boot - probe_boot);
                append(text, " mono_delta="); decimal(text + strlen(text), mono - probe_mono);
                append(text, " open="); decimal(text + strlen(text), now - probe_released);
                note(text);
                if (probe_open && now - probe_released < probe_open && probe_alarm)
                    arm_probe_alarm(probe_alarm);
                if (!probe_open || now - probe_released >= probe_open) {
                    strcpy(text, "nord2-power-probe: window closed after "); decimal(text + strlen(text), probe_cycles);
                    append(text, " cycles"); note(text);
                    power_snapshot();
                    if (!awake_write("/sys/power/wake_lock") && awake_held())
                        note("nord2-power-probe: diagnostic wake lock reacquired");
                    probe_hold = 0;
                }
            } else if (now - probe_released >= (probe_open ? probe_open + 60 : (probe_alarm ? probe_alarm + 20 : 25))) {
                strcpy(text, "nord2-power-probe: no suspend observed in "); decimal(text + strlen(text), now - probe_released);
                append(text, " seconds"); note(text);
                power_snapshot();
                if (!awake_write("/sys/power/wake_lock") && awake_held())
                    note("nord2-power-probe: diagnostic wake lock reacquired");
                probe_hold = 0;
            }
            flush_log();
        }
        /* Vendor AOD preference housekeeping and the two blank/unblank windows
         * the round measures: AOD off first, AOD on later, then restored. */
        if ((fix_aod || aod_test) && !fix1 && now - start >= 45) {
            fix1 = 1; note("nord2-aod-fix: saving and clearing vendor AOD preference"); flush_log();
            run_aod_save_and_clear();
        }
        if ((fix_aod || aod_test) && fix1 && !show1 && now - start >= 58) { show1 = 1; show_aod_fix(); }
        if (aod_test && !aod_on && now - start >= 150) {
            aod_on = 1; note("nord2-aod-fix: enabling vendor AOD for the second window"); flush_log(); run_aod_set("1");
        }
        if (aod_test && aod_on && !show2 && now - start >= 162) { show2 = 1; show_aod_fix(); }
        if ((fix_aod || aod_test) && !fix2 && now - start >= 205) {
            fix2 = 1; note("nord2-aod-fix: restoring vendor AOD preference"); flush_log(); run_aod_restore();
        }
        if ((fix_aod || aod_test) && fix2 && !show3 && now - start >= 218) { show3 = 1; show_aod_fix(); }
        if (now - start >= 40 && now - last_snapshot >= 40) {
            snapshot_file("/sys/class/power_supply/battery/uevent");
            snapshot_file("/sys/class/power_supply/usb/uevent");
            snapshot_file("/sys/class/leds/lcd-backlight/brightness");
            snapshot_file("/sys/class/leds/lcd-backlight/max_brightness");
            snapshot_file("/sys/class/leds/lcd-backlight/max_hw_brightness");
            snapshot_file("/sys/kernel/debug/dri/0/state");
            display_debug_command("diagnose");
            snapshot_file("/sys/kernel/debug/mtkfb");
            snapshot_file("/proc/interrupts");
            snapshot_file("/sys/kernel/debug/pinctrl/10005000.pinctrl/pinmux-pins");
            last_snapshot = now;
        }
        if (now != last) { flush_log(); last = now; }

        /* Service-crash diagnostics.
         *
         * ColorOS services die during the port's boot in a way stock does not reproduce,
         * and the ring buffer made this hard to see: 53.6% of one captured log was the
         * repeated ccci_fs EBUSY line (now ratelimited in port_proxy.c), which evicted the
         * dying services' own output.  With that noise gone, snapshot the evidence that
         * names a crashing service and its reason:
         *
         *   /sys/fs/pstore         kernel oops/panic records, if any survived
         *   init's service states   which units are in "restarting" and how often
         *   the tombstones          native crashes, with the signal and backtrace head
         *
         * Sampled twice so the difference shows what died after Android settled rather
         * than only what was mid-flight at one instant.
         */
        if (!svc_probe1 && now - start >= 120) {
            svc_probe1 = 1;
            note("nord2-svc: ---- service state at 120s (Android userspace up) ----");
            snapshot_file("/sys/fs/pstore/console-ramoops");
            snapshot_file("/proc/loadavg");
            run_svc_dump("120s");
        }
        if (!svc_probe2 && now - start >= 210) {
            svc_probe2 = 1;
            note("nord2-svc: ---- service state at 210s ----");
            run_svc_dump("210s");
        }
        if (!svc_probe3 && now - start >= 300) {
            svc_probe3 = 1;
            note("nord2-svc: ---- service state at 300s (steady state) ----");
            run_svc_dump("300s");
        }

        if (!requested && (now - start >= 360 || (!awake && now - start >= 30))) {
            note("nord2-bootguard: requesting normal Android recovery shutdown");
            flush_log(); request_recovery(); requested = 1;
        }
        if (now - start >= 420 || (!awake && now - start >= 45)) {
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
    /* switch_root discards the ramdisk, so latch the power-probe settings now. */
    int cfg = open("/nord2-power-probe", O_RDONLY | O_CLOEXEC);
    if (cfg >= 0) {
        ssize_t n = read(cfg, probe_cfg, sizeof(probe_cfg) - 1);
        close(cfg);
        if (n > 0) probe_cfg[n] = 0;
        puts("nord2-bootguard: power probe settings latched");
    }
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
