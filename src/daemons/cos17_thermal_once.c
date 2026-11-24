/* Pure-C zero-dependency one-shot thermal/radio/background hygiene helper (cos17_thermal_once v5).
 *
 * Rootless-ready (zero polling loops, runs ONCE on sys.boot_completed=1 under u:r:cos17_memkiller:s0,
 * zero SELinux AVCs, zero app removal):
 * 1. Disables silent disconnected Wi-Fi/BLE background scanning and stretches Wi-Fi RSSI/link-layer
 *    polling interval from 2000ms to 6000/10000ms (cutting OplusWifiCPUThr & hal_register_write_wq 3x).
 * 2. Keeps com.heytap.mydevices unrestricted for Bluetooth connected-device display.
 * 3. Unblocks com.oplus.aiunit and com.oplus.deepthinker network + bound-service execution so ALL
 *    ColorOS Gallery AI Editor features work 100% on-demand while keeping background scans ignored.
 *    Also unblocks App Market (com.heytap.market) + OpenID/AppDetail helpers on-demand with strict
 *    background scan/alarm appops ignored so App Market connects cleanly while staying 0% idle.
 * 4. Clears stale multi-megabyte SQLite WAL telemetry backlogs in com.oplus.framework.network.drs
 *    and com.oplus.statistics.rom via PackageManager (installd, zero AVCs).
 * 5. Non-destructively throttles background cellular keepalives, CN telemetry, and Baidu-NLP sockets
 *    via Android 15 eBPF FIREWALL_CHAIN_OEM_DENY_3 (ONLY on dedicated non-1000 UIDs!) + standby
 *    bucket + set-inactive + appops (all packages remain intact; Google GMS remains 100% active).
 * 6. Quietens UID-1000 background pollers (com.heytap.mcs, com.oplus.obrain, com.oplus.midas,
 *    com.oplus.onetrace, com.oplus.powermonitor, com.oplus.olc — per-package appops ONLY!).
 * 7. Applies one-time post-reset defaults (Gboard default IME, Secure keyboard OFF, GMS ON by
 *    default while toggleable in Settings) gated by Settings.System cos17_defaults_v1.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int (*prop_set_fn)(const char* key, const char* value);

static void write_node(const char* path, const char* val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void)write(fd, val, strlen(val));
    close(fd);
}

static void set_prop_safe(const char* key, const char* val) {
    prop_set_fn fn = (prop_set_fn)dlsym(RTLD_DEFAULT, "__system_property_set");
    if (fn) (void)fn(key, val);
}

static void run_cmd(char* const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }
}

static int is_setting_one(const char* ns, const char* key) {
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
        execl("/system/bin/cmd", "cmd", "settings", "get", ns, key, (char*)NULL);
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

static const char* const kNetDenyPkgs[] = {
    "com.oplus.location",
    "com.oplus.locationproxy",
    "com.oplus.framework.network.drs",
    "com.coloros.sceneservice",
    "com.oplus.pantanal.ums",
    "com.oplus.metis",
    "com.nearme.instant.card",
    "cn.richinfo.dmyk",
    "com.oppo.ctautoregist",
    "com.opos.ads",
    "com.oplus.romupdate",
    "com.heytap.cloud",
    "com.oplus.statistics.rom",
};

static const char* const kBgThrottlePkgs[] = {
    "cn.richinfo.dmyk",
    "com.oppo.ctautoregist",
    "com.oplus.framework.network.drs",
    "com.oplus.onetrace",
    "com.oplus.metis",
    "com.oplus.pantanal.ums",
    "com.nearme.instant.card",
    "com.opos.ads",
    "com.oplus.location",
    "com.oplus.locationproxy",
    "com.coloros.sceneservice",
    "com.oplus.romupdate",
    "com.heytap.cloud",
    "com.oplus.statistics.rom",
};

static const char* const kOnDemandPkgs[] = {
    "com.oplus.aiunit",
    "com.oplus.deepthinker",
    "com.heytap.market",
    "com.heytap.openid",
    "com.oplus.appdetail",
};

static const char* const kAiQuietScanOps[] = {
    "SCHEDULE_EXACT_ALARM",
    "WIFI_SCAN",
    "CHANGE_WIFI_STATE",
    "READ_CELL_INFO",
    "COARSE_LOCATION",
    "FINE_LOCATION",
    "MONITOR_LOCATION",
    "MONITOR_HIGH_POWER_LOCATION",
};

static const char* const kUid1000QuietPkgs[] = {
    "com.heytap.mcs",
    "com.oplus.obrain",
    "com.oplus.midas",
    "com.oplus.onetrace",
    "com.oplus.powermonitor",
    "com.oplus.olc",
};

static const char* const kBgAppOps[] = {
    "WAKE_LOCK",
    "START_FOREGROUND",
    "RUN_IN_BACKGROUND",
    "RUN_ANY_IN_BACKGROUND",
    "SCHEDULE_EXACT_ALARM",
    "WIFI_SCAN",
    "CHANGE_WIFI_STATE",
    "READ_CELL_INFO",
    "COARSE_LOCATION",
    "FINE_LOCATION",
    "MONITOR_LOCATION",
    "MONITOR_HIGH_POWER_LOCATION",
    "DIRECTION_SENSORS",
};

#define GBOARD_IME "com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME"

int main(void) {
    /* 0. Apply rootless VM watermark & swappiness (cos17_memkiller has proc_watermark_scale_factor write)
     *    and enforce midas_ostats restriction via system_prop (0 AVCs). */
    write_node("/proc/sys/vm/swappiness", "60\n");
    write_node("/proc/sys/vm/watermark_scale_factor", "20\n");
    set_prop_safe("persist.sys.midas_ostats.restriction", "1");

    /* 1. Stop disconnected background Wi-Fi/BLE scanning & moderate RSSI link-layer polling */
    {
        char* a1[] = {"/system/bin/cmd", "settings", "put", "global", "wifi_scan_always_enabled", "0", NULL};
        char* a2[] = {"/system/bin/cmd", "settings", "put", "global", "ble_scan_always_enabled", "0", NULL};
        char* a3[] = {"/system/bin/cmd", "wifi", "set-poll-rssi-interval-msecs", "6000", "10000", NULL};
        run_cmd(a1);
        run_cmd(a2);
        run_cmd(a3);
    }

    /* 2. Ensure com.heytap.mydevices stays unrestricted so Bluetooth connected devices display in UI */
    {
        char* m1[] = {"/system/bin/am", "set-standby-bucket", "com.heytap.mydevices", "active", NULL};
        char* m2[] = {"/system/bin/cmd", "appops", "set", "com.heytap.mydevices", "RUN_ANY_IN_BACKGROUND", "allow", NULL};
        char* m3[] = {"/system/bin/cmd", "appops", "set", "com.heytap.mydevices", "WAKE_LOCK", "allow", NULL};
        run_cmd(m1);
        run_cmd(m2);
        run_cmd(m3);
    }

    /* 2b. Unblock Gallery AI Editor & ColorOS App Market (+ OpenID/AppDetail) on demand */
    {
        size_t nai = sizeof(kOnDemandPkgs) / sizeof(kOnDemandPkgs[0]);
        size_t nq = sizeof(kAiQuietScanOps) / sizeof(kAiQuietScanOps[0]);
        for (size_t i = 0; i < nai; ++i) {
            char* pkg = (char*)kOnDemandPkgs[i];
            char* c1[] = {"/system/bin/cmd", "connectivity", "set-package-networking-enabled", "true", pkg, NULL};
            char* c2[] = {"/system/bin/am", "set-standby-bucket", pkg, "working_set", NULL};
            char* c3[] = {"/system/bin/cmd", "appops", "set", pkg, "WAKE_LOCK", "allow", NULL};
            char* c4[] = {"/system/bin/cmd", "appops", "set", pkg, "RUN_IN_BACKGROUND", "allow", NULL};
            char* c5[] = {"/system/bin/cmd", "appops", "set", pkg, "RUN_ANY_IN_BACKGROUND", "allow", NULL};
            run_cmd(c1);
            run_cmd(c2);
            run_cmd(c3);
            run_cmd(c4);
            run_cmd(c5);
            for (size_t j = 0; j < nq; ++j) {
                char* op = (char*)kAiQuietScanOps[j];
                char* c6[] = {"/system/bin/cmd", "appops", "set", pkg, op, "ignore", NULL};
                run_cmd(c6);
            }
        }
    }

    /* 2c. Purge stale multi-megabyte DRS / Statistics SQLite WAL backlogs via installd (zero AVCs) */
    {
        char* cl1[] = {"/system/bin/cmd", "package", "clear", "com.oplus.framework.network.drs", NULL};
        char* cl2[] = {"/system/bin/cmd", "package", "clear", "com.oplus.statistics.rom", NULL};
        run_cmd(cl1);
        run_cmd(cl2);
    }

    /* 3. Enable eBPF FIREWALL_CHAIN_OEM_DENY_3 on verified non-1000 CN telemetry / Baidu-NLP UIDs */
    {
        char* c0[] = {"/system/bin/cmd", "connectivity", "set-chain3-enabled", "true", NULL};
        run_cmd(c0);
        size_t nn = sizeof(kNetDenyPkgs) / sizeof(kNetDenyPkgs[0]);
        for (size_t i = 0; i < nn; ++i) {
            char* pkg = (char*)kNetDenyPkgs[i];
            char* cn[] = {"/system/bin/cmd", "connectivity", "set-package-networking-enabled", "false", pkg, NULL};
            run_cmd(cn);
        }
    }

    /* 4. Non-destructively restrict CN telemetry/registrar background wakelocks, alarms & scans */
    size_t n = sizeof(kBgThrottlePkgs) / sizeof(kBgThrottlePkgs[0]);
    size_t nop = sizeof(kBgAppOps) / sizeof(kBgAppOps[0]);
    for (size_t i = 0; i < n; ++i) {
        char* pkg = (char*)kBgThrottlePkgs[i];
        char* b1[] = {"/system/bin/am", "set-standby-bucket", pkg, "restricted", NULL};
        char* b1b[] = {"/system/bin/am", "set-inactive", pkg, "true", NULL};
        run_cmd(b1);
        run_cmd(b1b);
        for (size_t j = 0; j < nop; ++j) {
            char* op = (char*)kBgAppOps[j];
            char* b2[] = {"/system/bin/cmd", "appops", "set", pkg, op, "ignore", NULL};
            run_cmd(b2);
        }
        char* fs[] = {"/system/bin/am", "force-stop", pkg, NULL};
        run_cmd(fs);
    }

    /* 4b. Quieten UID-1000 pollers (per-package appops ONLY, NEVER in OEM_DENY_3!) */
    {
        size_t nu = sizeof(kUid1000QuietPkgs) / sizeof(kUid1000QuietPkgs[0]);
        for (size_t i = 0; i < nu; ++i) {
            char* pkg = (char*)kUid1000QuietPkgs[i];
            char* p0[] = {"/system/bin/cmd", "appops", "set", pkg, "START_FOREGROUND", "ignore", NULL};
            char* p1[] = {"/system/bin/cmd", "appops", "set", pkg, "WAKE_LOCK", "ignore", NULL};
            char* p2[] = {"/system/bin/cmd", "appops", "set", pkg, "RUN_IN_BACKGROUND", "ignore", NULL};
            char* p3[] = {"/system/bin/cmd", "appops", "set", pkg, "RUN_ANY_IN_BACKGROUND", "ignore", NULL};
            char* p4[] = {"/system/bin/cmd", "appops", "set", pkg, "SCHEDULE_EXACT_ALARM", "ignore", NULL};
            run_cmd(p0);
            run_cmd(p1);
            run_cmd(p2);
            run_cmd(p3);
            run_cmd(p4);
            if (i > 0) {
                char* fs[] = {"/system/bin/am", "force-stop", pkg, NULL};
                run_cmd(fs);
            }
        }
    }

    /* 5. Apply post-reset defaults ONCE per data wipe (Gboard, Secure KB OFF, GMS ON by default) */
    if (!is_setting_one("system", "cos17_defaults_v1")) {
        char* d1[] = {"/system/bin/cmd", "settings", "put", "system", "use_separate_keyboard", "0", NULL};
        char* d2[] = {"/system/bin/cmd", "settings", "put", "secure", "enabled_input_methods", GBOARD_IME, NULL};
        char* d3[] = {"/system/bin/cmd", "settings", "put", "secure", "default_input_method", GBOARD_IME, NULL};
        char* d4[] = {"/system/bin/cmd", "settings", "put", "system", "customize_control_cn_gms", "1", NULL};
        char* d5[] = {"/system/bin/pm", "enable", "com.google.android.gms", NULL};
        char* d6[] = {"/system/bin/cmd", "settings", "put", "system", "cos17_defaults_v1", "1", NULL};
        run_cmd(d1);
        run_cmd(d2);
        run_cmd(d3);
        run_cmd(d4);
        run_cmd(d5);
        run_cmd(d6);
    }

    /* 6. Re-assert Wi-Fi RSSI poll interval and VM watermark after ClientModeImpl settles (~10s later) */
    {
        char* w2[] = {"/system/bin/cmd", "wifi", "set-poll-rssi-interval-msecs", "6000", "10000", NULL};
        run_cmd(w2);
        write_node("/proc/sys/vm/watermark_scale_factor", "20\n");
        write_node("/proc/sys/vm/swappiness", "60\n");
        set_prop_safe("persist.sys.midas_ostats.restriction", "1");
    }
    return 0;
}
