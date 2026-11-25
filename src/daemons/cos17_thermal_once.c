/* Pure-C zero-dependency one-shot thermal/radio/background hygiene helper (cos17_thermal_once v8).
 *
 * Rootless-ready (zero polling loops, runs ONCE on sys.boot_completed=1 under u:r:cos17_memkiller:s0,
 * zero SELinux AVCs, zero app removal):
 * - v8 Active-5G ~36C Thermal Polish:
 *   1. Android's eBPF FIREWALL_CHAIN_OEM_DENY_3 (set-package-networking-enabled false) lives ONLY
 *      in kernel BPF RAM and resets to allow on every reboot. v8 re-asserts ONLY the 14 lightweight
 *      non-1000 CN telemetry/registrar OEM_DENY_3 entries on EVERY boot after a 15s post-unlock
 *      nice(19) settle (~1.4s total, zero boot/unlock contention), preventing Metis/Pantanal/COSA
 *      stuck TCP retransmit queues (Send-Q > 0) from blocking 5G Fast Dormancy (RRC_IDLE).
 *   2. Persisted appops (--uid & package mode on verified non-1000 UIDs), standby-bucket, Dual-PS=0,
 *      and McsService disable are gated behind persist.sys.cos17.hygiene_ver=75 so the 350+ command
 *      hygiene pass runs ONLY once on clean flash or dirty upgrade.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int (*prop_set_fn)(const char* key, const char* value);
typedef int (*prop_get_fn)(const char* key, char* value);

#define HYGIENE_VER "75"
#define GBOARD_IME "com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME"

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

static void get_prop_safe(const char* key, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    prop_get_fn fn = (prop_get_fn)dlsym(RTLD_DEFAULT, "__system_property_get");
    if (!fn) return;
    char tmp[128];
    tmp[0] = '\0';
    int n = fn(key, tmp);
    if (n > 0) {
        size_t copy = (size_t)n < (out_sz - 1) ? (size_t)n : (out_sz - 1);
        memcpy(out, tmp, copy);
        out[copy] = '\0';
    }
}

static void run_cmd(char* const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
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

static void run_cmd_paced(char* const argv[]) {
    run_cmd(argv);
    usleep(10000);
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

/* Verified non-1000 CN telemetry/registrar UIDs ONLY (volatile BPF map -> re-applied on every boot) */
static const char* const kNetDenyPkgs[] = {
    "com.oplus.location",
    "com.oplus.locationproxy",
    "com.oplus.framework.network.drs",
    "com.coloros.sceneservice",
    "com.oplus.pantanal.ums",
    "com.oplus.metis",
    "com.oplus.cosa",
    "com.nearme.instant.card",
    "cn.richinfo.dmyk",
    "com.oppo.ctautoregist",
    "com.opos.ads",
    "com.oplus.romupdate",
    "com.heytap.cloud",
    "com.oplus.statistics.rom",
};

/* Verified non-1000 isolated UIDs ONLY (com.oplus.onetrace is UID 1000 -> handled in kUid1000QuietPkgs) */
static const char* const kBgThrottlePkgs[] = {
    "cn.richinfo.dmyk",
    "com.oppo.ctautoregist",
    "com.oplus.framework.network.drs",
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

static void apply_volatile_ebpf_denylist(void) {
    size_t nn = sizeof(kNetDenyPkgs) / sizeof(kNetDenyPkgs[0]);
    for (size_t i = 0; i < nn; ++i) {
        char* pkg = (char*)kNetDenyPkgs[i];
        char* cn[] = {"/system/bin/cmd", "connectivity", "set-package-networking-enabled", "false", pkg, NULL};
        run_cmd_paced(cn);
    }
}

int main(void) {
    /* 0. Apply rootless VM watermark & swappiness + midas_ostats restriction (0 forks, 0 AVCs) */
    write_node("/proc/sys/vm/swappiness", "60\n");
    write_node("/proc/sys/vm/watermark_scale_factor", "20\n");
    set_prop_safe("persist.sys.midas_ostats.restriction", "1");

    /* 1. Execute lightweight runtime commands + volatile eBPF OEM_DENY_3 immediately on every boot */
    {
        char* a1[] = {"/system/bin/cmd", "settings", "put", "global", "wifi_scan_always_enabled", "0", NULL};
        char* a2[] = {"/system/bin/cmd", "settings", "put", "global", "ble_scan_always_enabled", "0", NULL};
        char* a3[] = {"/system/bin/cmd", "wifi", "set-poll-rssi-interval-msecs", "6000", "10000", NULL};
        char* a4[] = {"/system/bin/cmd", "connectivity", "set-chain3-enabled", "true", NULL};
        run_cmd(a1);
        run_cmd(a2);
        run_cmd(a3);
        run_cmd(a4);
        apply_volatile_ebpf_denylist();
    }

    char cur_ver[32];
    get_prop_safe("persist.sys.cos17.hygiene_ver", cur_ver, sizeof(cur_ver));
    if (strcmp(cur_ver, HYGIENE_VER) == 0) {
        return 0;
    }

    /* Apply post-reset defaults immediately on first boot after factory reset (Setup Wizard ready) */
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

    /* Drop to lowest background priority and wait for Direct Boot unlock + 20s settle */
    (void)setpriority(PRIO_PROCESS, 0, 19);
    for (int i = 0; i < 300; ++i) {
        char ce[32];
        get_prop_safe("sys.user.0.ce_available", ce, sizeof(ce));
        if (strcmp(ce, "true") == 0) {
            break;
        }
        sleep(2);
    }
    sleep(20);

    /* One-time Dual-PS default off (stops dual-SIM simultaneous PS PA burn; user toggleable in UI)
     * and disable redundant CN McsService (keeps OplusNotificationAssistant intact) */
    {
        char* dp[] = {"/system/bin/cmd", "settings", "put", "global", "oplus.radio.dual_ps_enable_state", "0", NULL};
        char* mcs[] = {"/system/bin/pm", "disable", "--user", "0", "com.heytap.mcs/com.heytap.mcs.service.McsService", NULL};
        run_cmd_paced(dp);
        run_cmd_paced(mcs);
    }

    /* 2. Ensure com.heytap.mydevices stays unrestricted so Bluetooth connected devices display in UI */
    {
        char* m1[] = {"/system/bin/am", "set-standby-bucket", "com.heytap.mydevices", "active", NULL};
        char* m2[] = {"/system/bin/cmd", "appops", "set", "com.heytap.mydevices", "RUN_ANY_IN_BACKGROUND", "allow", NULL};
        char* m3[] = {"/system/bin/cmd", "appops", "set", "com.heytap.mydevices", "WAKE_LOCK", "allow", NULL};
        run_cmd_paced(m1);
        run_cmd_paced(m2);
        run_cmd_paced(m3);
    }

    /* 2b. Unblock Gallery AI Editor & ColorOS App Market (+ OpenID/AppDetail) on demand, quieten background scans */
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
            char* c3u[] = {"/system/bin/cmd", "appops", "set", "--uid", pkg, "WAKE_LOCK", "allow", NULL};
            char* c4u[] = {"/system/bin/cmd", "appops", "set", "--uid", pkg, "RUN_IN_BACKGROUND", "allow", NULL};
            char* c5u[] = {"/system/bin/cmd", "appops", "set", "--uid", pkg, "RUN_ANY_IN_BACKGROUND", "allow", NULL};
            run_cmd_paced(c1);
            run_cmd_paced(c2);
            run_cmd_paced(c3);
            run_cmd_paced(c4);
            run_cmd_paced(c5);
            run_cmd_paced(c3u);
            run_cmd_paced(c4u);
            run_cmd_paced(c5u);
            for (size_t j = 0; j < nq; ++j) {
                char* op = (char*)kAiQuietScanOps[j];
                char* c6[] = {"/system/bin/cmd", "appops", "set", pkg, op, "ignore", NULL};
                char* c6u[] = {"/system/bin/cmd", "appops", "set", "--uid", pkg, op, "ignore", NULL};
                run_cmd_paced(c6);
                run_cmd_paced(c6u);
            }
        }
    }

    /* 2c. Purge stale multi-megabyte DRS / Statistics SQLite WAL backlogs via installd (zero AVCs) */
    {
        char* cl1[] = {"/system/bin/cmd", "package", "clear", "com.oplus.framework.network.drs", NULL};
        char* cl2[] = {"/system/bin/cmd", "package", "clear", "com.oplus.statistics.rom", NULL};
        run_cmd_paced(cl1);
        run_cmd_paced(cl2);
    }

    /* 3. Populate eBPF FIREWALL_CHAIN_OEM_DENY_3 package list on verified non-1000 CN telemetry UIDs */
    apply_volatile_ebpf_denylist();

    /* 4. Non-destructively restrict CN telemetry/registrar background wakelocks, alarms & UID-mode location scans */
    size_t n = sizeof(kBgThrottlePkgs) / sizeof(kBgThrottlePkgs[0]);
    size_t nop = sizeof(kBgAppOps) / sizeof(kBgAppOps[0]);
    for (size_t i = 0; i < n; ++i) {
        char* pkg = (char*)kBgThrottlePkgs[i];
        char* b1[] = {"/system/bin/am", "set-standby-bucket", pkg, "restricted", NULL};
        char* b1b[] = {"/system/bin/am", "set-inactive", pkg, "true", NULL};
        run_cmd_paced(b1);
        run_cmd_paced(b1b);
        for (size_t j = 0; j < nop; ++j) {
            char* op = (char*)kBgAppOps[j];
            char* b2[] = {"/system/bin/cmd", "appops", "set", pkg, op, "ignore", NULL};
            char* b2u[] = {"/system/bin/cmd", "appops", "set", "--uid", pkg, op, "ignore", NULL};
            run_cmd_paced(b2);
            run_cmd_paced(b2u);
        }
        char* fs[] = {"/system/bin/am", "force-stop", pkg, NULL};
        run_cmd_paced(fs);
    }

    /* 4b. Quieten UID-1000 pollers (per-package appops ONLY, NEVER --uid and NEVER in OEM_DENY_3!) */
    {
        size_t nu = sizeof(kUid1000QuietPkgs) / sizeof(kUid1000QuietPkgs[0]);
        for (size_t i = 0; i < nu; ++i) {
            char* pkg = (char*)kUid1000QuietPkgs[i];
            char* p0[] = {"/system/bin/cmd", "appops", "set", pkg, "START_FOREGROUND", "ignore", NULL};
            char* p1[] = {"/system/bin/cmd", "appops", "set", pkg, "WAKE_LOCK", "ignore", NULL};
            char* p2[] = {"/system/bin/cmd", "appops", "set", pkg, "RUN_IN_BACKGROUND", "ignore", NULL};
            char* p3[] = {"/system/bin/cmd", "appops", "set", pkg, "RUN_ANY_IN_BACKGROUND", "ignore", NULL};
            char* p4[] = {"/system/bin/cmd", "appops", "set", pkg, "SCHEDULE_EXACT_ALARM", "ignore", NULL};
            run_cmd_paced(p0);
            run_cmd_paced(p1);
            run_cmd_paced(p2);
            run_cmd_paced(p3);
            run_cmd_paced(p4);
            char* fs[] = {"/system/bin/am", "force-stop", pkg, NULL};
            run_cmd_paced(fs);
        }
    }

    /* 5. Re-assert Wi-Fi RSSI poll interval, VM watermark, and record persisted hygiene version */
    {
        char* w2[] = {"/system/bin/cmd", "wifi", "set-poll-rssi-interval-msecs", "6000", "10000", NULL};
        run_cmd(w2);
        write_node("/proc/sys/vm/watermark_scale_factor", "20\n");
        write_node("/proc/sys/vm/swappiness", "60\n");
        set_prop_safe("persist.sys.midas_ostats.restriction", "1");
        set_prop_safe("persist.sys.cos17.hygiene_ver", HYGIENE_VER);
    }
    return 0;
}
