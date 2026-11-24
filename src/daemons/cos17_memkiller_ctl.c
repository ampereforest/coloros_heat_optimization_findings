/* Pure C zero-dependency cos17_memkiller_ctl (NO libc++_shared.so, ZERO SELinux AVCs) */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

typedef int (*prop_set_fn)(const char* key, const char* value);
static prop_set_fn g_prop_set = NULL;

static void set_prop(const char* k, const char* v) {
    if (g_prop_set) (void)g_prop_set(k, v);
}

static int get_prop_int(const char* k, int defval) {
    char buf[PROP_VALUE_MAX];
    memset(buf, 0, sizeof(buf));
    if (__system_property_get(k, buf) <= 0) return defval;
    char* end = NULL;
    long v = strtol(buf, &end, 10);
    if (!end || *end != '\0') return defval;
    return (int)v;
}

int main(void) {
    g_prop_set = (prop_set_fn)dlsym(RTLD_DEFAULT, "__system_property_set");
    if (!g_prop_set) return 1;

    int enabled = get_prop_int("persist.sys.oplus.disable_mem_killer", 0);
    int floor_mb = get_prop_int("persist.sys.oplus.mem_killer_floor_mb", 500);
    if (floor_mb < 0) floor_mb = 0;
    if (floor_mb > 500) floor_mb = 500;

    if (enabled == 1) {
        int p950 = floor_mb * 256;
        int p900 = p950 * 7 / 10;
        int p250 = floor_mb * 48;
        int p200 = floor_mb * 36;
        int p100 = floor_mb * 24;
        int p0   = floor_mb * 12;

        char levels[128];
        snprintf(levels, sizeof(levels), "%d:0,%d:100,%d:200,%d:250,%d:900,%d:950",
                 p0, p100, p200, p250, p900, p950);

        set_prop("persist.sys.lmk.oplus.kill_memleak_process", "0");
        if (floor_mb == 0) {
            set_prop("persist.device_config.lmkd_native.lowmem_min_oom_score", "1001");
            set_prop("persist.device_config.lmkd_native.psi_partial_stall_ms", "60000");
            set_prop("persist.device_config.lmkd_native.psi_complete_stall_ms", "60000");
            set_prop("persist.device_config.lmkd_native.thrashing_limit", "10000");
            set_prop("persist.device_config.lmkd_native.thrashing_limit_critical", "10000");
            set_prop("persist.device_config.lmkd_native.thrashing_limit_decay", "0");
            set_prop("persist.device_config.lmkd_native.swap_free_low_percentage", "0");
            set_prop("persist.device_config.lmkd_native.kill_timeout_ms", "60000");
            set_prop("persist.device_config.lmkd_native.filecache_min_kb", "0");
        } else {
            set_prop("persist.device_config.lmkd_native.lowmem_min_oom_score", "950");
            set_prop("persist.device_config.lmkd_native.psi_partial_stall_ms", "2000");
            set_prop("persist.device_config.lmkd_native.psi_complete_stall_ms", "3000");
            set_prop("persist.device_config.lmkd_native.thrashing_limit", "200");
            set_prop("persist.device_config.lmkd_native.thrashing_limit_critical", "500");
            set_prop("persist.device_config.lmkd_native.thrashing_limit_decay", "10");
            set_prop("persist.device_config.lmkd_native.swap_free_low_percentage", "2");
            set_prop("persist.device_config.lmkd_native.kill_timeout_ms", "200");
            set_prop("persist.device_config.lmkd_native.filecache_min_kb", "");
        }
        set_prop("sys.lmk.minfree_levels", levels);
    } else {
        set_prop("persist.sys.lmk.oplus.kill_memleak_process", "1");
        set_prop("persist.device_config.lmkd_native.lowmem_min_oom_score", "");
        set_prop("persist.device_config.lmkd_native.psi_partial_stall_ms", "");
        set_prop("persist.device_config.lmkd_native.psi_complete_stall_ms", "");
        set_prop("persist.device_config.lmkd_native.thrashing_limit", "");
        set_prop("persist.device_config.lmkd_native.thrashing_limit_critical", "300");
        set_prop("persist.device_config.lmkd_native.thrashing_limit_decay", "");
        set_prop("persist.device_config.lmkd_native.swap_free_low_percentage", "");
        set_prop("persist.device_config.lmkd_native.kill_timeout_ms", "");
        set_prop("persist.device_config.lmkd_native.filecache_min_kb", "");
        set_prop("sys.lmk.minfree_levels", "1200:0,2400:100,3600:200,4800:250,17920:900,25600:950");
    }

    return 0;
}
