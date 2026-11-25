/* Pure-C zero-dependency post-unlock ART speed-profile compiler (cos17_dexopt_once v2).
 *
 * Rootless-ready (zero libc++_shared.so, runs under u:r:cos17_memkiller:s0, zero SELinux AVCs):
 * - Fixes v1 bug where /data/system/.cos17_dexopt_done failed with -EACCES under cos17_memkiller
 *   and pre-unlock execution missed FBE-encrypted /data/misc/profiles/cur/0/<pkg> profiles, leaving
 *   patched deodexed SystemUI.apk & Settings.apk stuck in [status=run-from-apk].
 * - Waits for Direct Boot unlock (sys.user.0.ce_available=true) + 25s nice(19) settle, binds
 *   all 3 stdio FDs (0, 1, 2) to /dev/null owned by cos17_memkiller (0 Binder FD AVCs), compiles
 *   SystemUI & SmartSideBar with -m speed and 8 core ROM packages with -m speed-profile, and gates
 *   via persist.sys.cos17.dexopt_ver=75 ONLY after critical packages return exit code 0 (covering
 *   both clean flashes and dirty upgrades from v7.1/v7.2 with zero repeat overhead on normal boots).
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

#define DEXOPT_VER "75"

static const char* const kTargetPackages[] = {
    "com.android.systemui",
    "com.coloros.smartsidebar",
    "com.android.settings",
    "com.android.launcher",
    "com.oplus.uxdesign",
    "com.oplus.wallpapers",
    "com.android.camera",
    "com.coloros.gallery3d",
    "com.oplus.wirelesssettings",
    "com.dada.coloros.esim",
};

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

static int compile_pkg(const char* pkg, const char* mode) {
    pid_t pid = fork();
    if (pid == 0) {
        (void)setpriority(PRIO_PROCESS, 0, 19);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execl("/system/bin/cmd", "cmd", "package", "compile",
              "-m", mode, "-f", pkg, (char*)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
    }
    return -1;
}

int main(void) {
    char cur_ver[32];
    get_prop_safe("persist.sys.cos17.dexopt_ver", cur_ver, sizeof(cur_ver));
    if (strcmp(cur_ver, DEXOPT_VER) == 0) {
        return 0;
    }

    (void)setpriority(PRIO_PROCESS, 0, 19);

    /* Wait for Direct Boot CE unlock so /data/misc/profiles/cur/0/<pkg> is accessible */
    for (int i = 0; i < 300; ++i) {
        char ce[32];
        get_prop_safe("sys.user.0.ce_available", ce, sizeof(ce));
        if (strcmp(ce, "true") == 0) {
            break;
        }
        sleep(2);
    }

    /* Allow post-unlock UI & initial JIT profile flush to settle */
    sleep(25);

    int critical_ok = 1;
    size_t count = sizeof(kTargetPackages) / sizeof(kTargetPackages[0]);
    for (size_t i = 0; i < count; ++i) {
        /* Patched SystemUI.apk & SmartSideBar.apk use full -m speed (~12s total) so QS,
         * NotificationShade, Blur & Smart Sidebar 'All' drawer are 100% AOT-compiled; others speed-profile */
        const char* mode = (i < 2) ? "speed" : "speed-profile";
        int rc = compile_pkg(kTargetPackages[i], mode);
        if (i < 3 && rc != 0) {
            critical_ok = 0;
        }
    }

    if (critical_ok) {
        set_prop_safe("persist.sys.cos17.dexopt_ver", DEXOPT_VER);
    }
    return 0;
}
