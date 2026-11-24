/* Pure-C zero-dependency cos17_dexopt_once (Setup-Wizard-safe + thermal-gated + LITTLE-core pinned)
 *
 * Fixes Setup Wizard (OplusBootReg) overheating after format data:
 * 1. If Settings.Global `cos17_dexopt_done` == 1 -> instant exit (0 ms overhead, zero /data file AVCs).
 * 2. Checks Global setting `device_provisioned` ONCE (no polling loop):
 *    If != 1 (Setup Wizard active), exits immediately WITHOUT compiling or setting marker.
 * 3. On the first normal boot AFTER Setup Wizard completes:
 *    - Waits 120s for boot & user unlock to settle.
 *    - Pins to efficiency/mid cores (CPU 0..5, never prime cores 6..7) + nice(19).
 *    - Checks board_sensor_temp < 38000 mC before each package; if warm, exits cleanly
 *      to finish on next cool boot rather than heating the device.
 *    - Sets Settings.Global `cos17_dexopt_done = 1` only after completion.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char* const kBoardTempPath = "/sys/class/thermal/thermal_message/board_sensor_temp";
static const int kMaxBoardTempMc = 38000; /* 38.0 C */

static const char* const kTargetPackages[] = {
    "com.oplus.uxdesign",
    "com.oplus.wallpapers",
    "com.android.camera",
    "com.coloros.gallery3d",
    "com.android.settings",
    "com.android.launcher",
    "com.android.systemui",
    "com.oplus.wirelesssettings",
    "com.dada.coloros.esim",
};

static int read_int_file(const char* path, int fallback) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fallback;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return fallback;
    buf[n] = '\0';
    return atoi(buf);
}

static int is_global_setting_one(const char* key) {
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execl("/system/bin/cmd", "cmd", "settings", "get", "global", key, (char*)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char buf[32];
    ssize_t n = 0;
    if (pid > 0) {
        n = read(pfd[0], buf, sizeof(buf) - 1);
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
    close(pfd[0]);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (buf[0] == '1');
}

static void set_global_setting_one(const char* key) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execl("/system/bin/cmd", "cmd", "settings", "put", "global", key, "1", (char*)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
}

static void pin_efficiency_cores(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c = 0; c < 6; ++c) {
        CPU_SET(c, &set);
    }
    (void)sched_setaffinity(0, sizeof(set), &set);
}

static void compile_pkg(const char* pkg) {
    pid_t pid = fork();
    if (pid == 0) {
        pin_efficiency_cores();
        (void)nice(19);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execl("/system/bin/cmd", "cmd", "package", "compile",
              "-m", "speed-profile", "-f", pkg, (char*)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
}

int main(void) {
    /* Strict one-time guard: zero overhead on all subsequent boots */
    if (is_global_setting_one("cos17_dexopt_done")) {
        return 0;
    }

    /* NEVER run during Setup Wizard (OplusBootReg) after wipe data */
    if (!is_global_setting_one("device_provisioned")) {
        return 0;
    }

    pin_efficiency_cores();
    (void)nice(19);

    /* Wait 120s so post-boot services finish settling */
    sleep(120);

    size_t count = sizeof(kTargetPackages) / sizeof(kTargetPackages[0]);
    for (size_t i = 0; i < count; ++i) {
        if (read_int_file(kBoardTempPath, 0) >= kMaxBoardTempMc) {
            /* Yield cleanly if device is warm; retry on next cool boot */
            return 0;
        }
        compile_pkg(kTargetPackages[i]);
        sleep(5);
    }

    set_global_setting_one("cos17_dexopt_done");
    return 0;
}
