#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define DISP_PARAM_PATH "/sys/class/mi_display/disp-DSI-0/disp_param"
#define BL_PATH         "/sys/class/backlight/panel0-backlight/brightness"
#define BLP_PATH        "/sys/class/backlight/panel0-backlight/bl_power"

#define SF_SERVICE      "SurfaceFlingerAIDL"
#define SF_DESC         "android.gui.ISurfaceComposer"
#define HDR_LISTENER_DESC "android.gui.IHdrLayerInfoListener"
#define DF_SERVICE      "vendor.xiaomi.hardware.displayfeature_aidl.IDisplayFeature/default"
#define DF_DESC         "vendor.xiaomi.hardware.displayfeature_aidl.IDisplayFeature"

enum {
    TX_SF_GET_PHYS_IDS     = 0x6,
    TX_SF_GET_PHYS_TOKEN   = 0x7,
    TX_SF_ADD_HDR_LISTENER = 0x38,
    TX_SF_REM_HDR_LISTENER = 0x39,
    TX_HDR_ON_CHANGED      = FIRST_CALL_TRANSACTION,
    TX_DF_SET_FEATURE      = 0x7,
};

static constexpr int HW_PEAK_DBV   = 8191;
static constexpr int SDR_MAX_DBV   = 2173;
static constexpr int SDR_BOOST_CAP = 6200;
static constexpr int EDR_BUMP_MAX  = 96;
static constexpr int RAMP_STEPS    = 6;

typedef AIBinder* (*wait_service_fn)(const char* instance);
typedef AIBinder* (*check_service_fn)(const char* instance);
typedef void (*set_threads_fn)(uint32_t numThreads);
typedef void (*void_fn)(void);

static wait_service_fn  g_wait_service  = nullptr;
static check_service_fn g_check_service = nullptr;
static set_threads_fn   g_set_threads   = nullptr;
static void_fn          g_start_pool    = nullptr;

static AIBinder_Class*  g_sf_cls        = nullptr;
static AIBinder_Class*  g_df_cls        = nullptr;

static int g_fd_disp = -1;
static int g_fd_bl   = -1;
static int g_fd_blp  = -1;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t g_stop = 0;

// Shared state (protected by g_lock)
static int   g_hdr_layers = 0;
static float g_hdr_ratio  = 1.0f;
static int   g_peak_level = 100;
static int   g_peak_dbv   = HW_PEAK_DBV;
static int   g_boost_en   = 0;
static int   g_boost_lvl  = 50;
static bool  g_keyguard   = false;
static bool  g_dirty      = true;

// Worker state
static int g_active       = 0;
static int g_sdr_dbv      = 0;
static int g_last22       = -1;
static int g_last_df_tier = -1;

static AIBinder* g_df_binder = nullptr;

// Panel o3_42_0d_0a calibrated DBV <-> luminance (nits x10)
// Anchors: DBV 7=2.0 nits, 2173=722.6 nits (SDR max), 6142=1800.0 nits (HBM), 8191=3200.0 nits (Peak HDR)
static inline int nits10_of_dbv(int d) {
    if (d <= 7) return 20;
    if (d <= 2173) return 20 + (d - 7) * 7206 / 2166;
    if (d <= 6142) return 7226 + (d - 2173) * 10774 / 3969;
    return 18000 + (d - 6142) * 14000 / 2049;
}

static inline int dbv_of_nits10(int n) {
    if (n <= 20) return 7;
    if (n <= 7226) return 7 + (n - 20) * 2166 / 7206;
    if (n <= 18000) return 2173 + (n - 7226) * 3969 / 10774;
    if (n >= 32000) return HW_PEAK_DBV;
    return 6142 + (n - 18000) * 2049 / 14000;
}

static inline int clamp_sdr(int v) {
    if (v < 0) return 0;
    return v > SDR_MAX_DBV ? SDR_MAX_DBV : v;
}

static int pread_int(int fd, int fallback) {
    if (fd < 0) return fallback;
    char buf[32];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return fallback;
    buf[n] = '\0';
    return atoi(buf);
}

static bool write_dbv(int val) {
    if (g_fd_disp < 0) return false;
    if (val == g_last22) return true;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "22 %d\n", val);
    if (len <= 0) return false;
    ssize_t n = pwrite(g_fd_disp, buf, static_cast<size_t>(len), 0);
    if (n == len) {
        g_last22 = val;
        return true;
    }
    return false;
}

static void write_bl(int val) {
    if (g_fd_bl < 0) return;
    char buf[24];
    int len = snprintf(buf, sizeof(buf), "%d\n", val);
    if (len > 0) (void)pwrite(g_fd_bl, buf, static_cast<size_t>(len), 0);
}

static void* noop_on_create(void* args) { return args; }
static void  noop_on_destroy(void*) {}
static binder_status_t noop_on_transact(
        AIBinder*, transaction_code_t, const AParcel*, AParcel*) {
    return STATUS_UNKNOWN_TRANSACTION;
}

static AIBinder* get_df_binder() {
    if (g_df_binder && AIBinder_isAlive(g_df_binder)) return g_df_binder;
    if (g_df_binder) {
        AIBinder_decStrong(g_df_binder);
        g_df_binder = nullptr;
    }
    if (g_check_service && g_df_cls) {
        AIBinder* b = g_check_service(DF_SERVICE);
        if (b) {
            if (AIBinder_associateClass(b, g_df_cls)) {
                g_df_binder = b;
            } else {
                AIBinder_decStrong(b);
            }
        }
    }
    return g_df_binder;
}

static void df_set_feature(int caseId, int modeId, int cookie = 255) {
    AIBinder* b = get_df_binder();
    if (!b) return;
    AParcel* in = nullptr;
    AParcel* out = nullptr;
    if (AIBinder_prepareTransaction(b, &in) != STATUS_OK) return;
    AParcel_writeInt32(in, 0);       // displayId
    AParcel_writeInt32(in, caseId);
    AParcel_writeInt32(in, modeId);
    AParcel_writeInt32(in, cookie);
    AIBinder_transact(b, TX_DF_SET_FEATURE, &in, &out, 0);
    if (out) AParcel_delete(out);
}

static int compute_df_tier(int on_now, int hdr_now, int peak_lvl, int boost_en, int boost_lvl) {
    if (!on_now || !hdr_now) return 0;
    if (peak_lvl >= 120) return 3;
    if (peak_lvl >= 110) return 2;
    if (peak_lvl > 100 || (boost_en && boost_lvl > 0)) return 1;
    return 0;
}

static void apply_df_profile(int tier, int hdr_active) {
    if (tier == g_last_df_tier) return;
    g_last_df_tier = tier;
    switch (tier) {
        case 0:
            df_set_feature(70, 0);
            df_set_feature(19, 0);
            df_set_feature(48, 0);
            df_set_feature(1, 0);
            df_set_feature(27, 0);
            df_set_feature(51, 0);
            if (!hdr_active) df_set_feature(10, 0);
            break;
        case 1:
            df_set_feature(70, 0);
            df_set_feature(19, 0);
            df_set_feature(48, 0);
            df_set_feature(1, 0);
            if (!hdr_active) df_set_feature(10, 0);
            df_set_feature(51, 1);
            df_set_feature(27, 1);
            break;
        case 2:
            df_set_feature(70, 0);
            df_set_feature(19, 0);
            if (!hdr_active) df_set_feature(10, 0);
            df_set_feature(51, 1);
            df_set_feature(27, 1);
            df_set_feature(1, 1);
            df_set_feature(48, 1);
            break;
        default:
            df_set_feature(51, 1);
            df_set_feature(27, 1);
            df_set_feature(1, 1);
            df_set_feature(48, 1);
            df_set_feature(10, 1);
            df_set_feature(19, 1);
            df_set_feature(70, 1);
            break;
    }
}

static int sdr_target_for(int base, int boost_en, int boost_lvl, int peak_lvl) {
    if (!boost_en || boost_lvl <= 0) return base;
    int g = 100 + boost_lvl * peak_lvl / 100;
    int out = dbv_of_nits10(nits10_of_dbv(base) * g / 100);
    if (out > SDR_BOOST_CAP) out = SDR_BOOST_CAP;
    if (out < base) out = base;
    return out;
}

static int peak_target_for(int sdr_dbv, float ratio, int peak_lvl, int peak_dbv) {
    int rx100 = static_cast<int>(lroundf(ratio * 100.0f));
    if (rx100 < 100) rx100 = 100;
    if (peak_lvl > 100) rx100 = rx100 * peak_lvl / 100;
    long long target_nits10 = static_cast<long long>(nits10_of_dbv(sdr_dbv)) * rx100 / 100;
    if (target_nits10 > 32000) target_nits10 = 32000;
    int p = dbv_of_nits10(static_cast<int>(target_nits10));
    if (p > peak_dbv) p = peak_dbv;
    if (p < sdr_dbv) p = sdr_dbv;
    return p;
}

static int read_prop_int(const char* name, int defval) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) <= 0) return defval;
    char* end = nullptr;
    long v = strtol(buf, &end, 10);
    if (!end || *end != '\0') return defval;
    return static_cast<int>(v);
}

static void refresh_prefs_locked() {
    int pl = read_prop_int("persist.sys.oplus.hdr_peak_level", 100);
    if (pl < 50) pl = 50;
    if (pl > 125) pl = 125;
    g_peak_level = pl;
    if (pl <= 75) {
        g_peak_dbv = 2800 + (pl - 50) * 2400 / 25;
    } else if (pl <= 100) {
        g_peak_dbv = 5200 + (pl - 75) * (HW_PEAK_DBV - 5200) / 25;
    } else {
        g_peak_dbv = HW_PEAK_DBV;
    }
    if (g_peak_dbv > HW_PEAK_DBV) g_peak_dbv = HW_PEAK_DBV;

    g_boost_en = (read_prop_int("persist.sys.oplus.sdr2hdr_enable", 0) == 1) ? 1 : 0;
    int blv = read_prop_int("persist.sys.oplus.sdr2hdr_level", 50);
    if (blv < 0) blv = 0;
    if (blv > 100) blv = 100;
    g_boost_lvl = blv;

    char kg[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.sys.oplus.keyguard.showing", kg);
    g_keyguard = (strcmp(kg, "true") == 0);
}

// Per-property futex waiter: blocks in kernel at 0.000% CPU until the exact property changes
static void* prop_watch_thread(void* arg) {
    const char* prop_name = static_cast<const char*>(arg);
    const prop_info* pi = nullptr;
    uint32_t serial = 0;
    uint32_t global_ser = __system_property_area_serial();
    while (!g_stop) {
        if (!pi) {
            pi = __system_property_find(prop_name);
            if (!pi) {
                uint32_t new_global = global_ser;
                __system_property_wait(nullptr, global_ser, &new_global, nullptr);
                global_ser = new_global;
                continue;
            }
            serial = __system_property_serial(pi);
            pthread_mutex_lock(&g_lock);
            refresh_prefs_locked();
            g_dirty = true;
            pthread_cond_signal(&g_cond);
            pthread_mutex_unlock(&g_lock);
        }
        uint32_t new_serial = serial;
        __system_property_wait(pi, serial, &new_serial, nullptr);
        serial = new_serial;

        pthread_mutex_lock(&g_lock);
        refresh_prefs_locked();
        g_dirty = true;
        pthread_cond_signal(&g_cond);
        pthread_mutex_unlock(&g_lock);
    }
    return nullptr;
}

// Binder callback: IHdrLayerInfoListener::onHdrLayerInfoChanged
static binder_status_t listener_on_transact(
        AIBinder*, transaction_code_t code, const AParcel* in, AParcel*) {
    if (code != TX_HDR_ON_CHANGED) return STATUS_UNKNOWN_TRANSACTION;
    int32_t num_layers = 0, max_w = 0, max_h = 0, flags = 0;
    float max_ratio = 1.0f;
    AParcel_readInt32(in, &num_layers);
    AParcel_readInt32(in, &max_w);
    AParcel_readInt32(in, &max_h);
    AParcel_readInt32(in, &flags);
    AParcel_readFloat(in, &max_ratio);

    if (num_layers <= 0 || max_ratio < 1.5f) {
        num_layers = 0;
        max_ratio = 1.0f;
    }

    pthread_mutex_lock(&g_lock);
    if (num_layers != g_hdr_layers || fabsf(max_ratio - g_hdr_ratio) > 0.02f) {
        g_hdr_layers = num_layers;
        g_hdr_ratio = max_ratio;
        g_dirty = true;
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
    return STATUS_OK;
}

static bool register_hdr_listener(AIBinder* listener) {
    if (!g_wait_service || !g_sf_cls) return false;
    AIBinder* sf = g_wait_service(SF_SERVICE);
    if (!sf) return false;
    if (!AIBinder_associateClass(sf, g_sf_cls)) {
        AIBinder_decStrong(sf);
        return false;
    }

    // 1. getPhysicalDisplayIds
    AParcel* in = nullptr;
    AParcel* out = nullptr;
    int64_t display_id = 0;
    bool got_id = false;
    if (AIBinder_prepareTransaction(sf, &in) == STATUS_OK &&
        AIBinder_transact(sf, TX_SF_GET_PHYS_IDS, &in, &out, 0) == STATUS_OK && out) {
        int32_t exc = 0, count = 0;
        if (AParcel_readInt32(out, &exc) == STATUS_OK && exc == 0 &&
            AParcel_readInt32(out, &count) == STATUS_OK && count > 0) {
            if (AParcel_readInt64(out, &display_id) == STATUS_OK) got_id = true;
        }
        AParcel_delete(out);
    }
    if (!got_id) {
        AIBinder_decStrong(sf);
        return false;
    }

    // 2. getPhysicalDisplayToken(display_id)
    AIBinder* token = nullptr;
    in = nullptr; out = nullptr;
    if (AIBinder_prepareTransaction(sf, &in) == STATUS_OK) {
        AParcel_writeInt64(in, display_id);
        if (AIBinder_transact(sf, TX_SF_GET_PHYS_TOKEN, &in, &out, 0) == STATUS_OK && out) {
            int32_t exc = 0;
            if (AParcel_readInt32(out, &exc) == STATUS_OK && exc == 0) {
                AParcel_readStrongBinder(out, &token);
            }
            AParcel_delete(out);
        }
    }
    if (!token) {
        AIBinder_decStrong(sf);
        return false;
    }

    // 3. addHdrLayerInfoListener(token, listener)
    bool ok = false;
    in = nullptr; out = nullptr;
    if (AIBinder_prepareTransaction(sf, &in) == STATUS_OK) {
        AParcel_writeStrongBinder(in, token);
        AParcel_writeStrongBinder(in, listener);
        if (AIBinder_transact(sf, TX_SF_ADD_HDR_LISTENER, &in, &out, 0) == STATUS_OK && out) {
            int32_t exc = 0;
            if (AParcel_readInt32(out, &exc) == STATUS_OK && exc == 0) ok = true;
            AParcel_delete(out);
        }
    }

    AIBinder_decStrong(token);
    AIBinder_decStrong(sf);
    return ok;
}

static void ramp_dbv(int from, int to) {
    if (to <= from) {
        write_dbv(to);
        return;
    }
    struct timespec ts = {0, 10 * 1000 * 1000}; // 10ms
    for (int i = 1; i <= RAMP_STEPS && !g_stop; ++i) {
        if (pread_int(g_fd_blp, 4) != 0 || pread_int(g_fd_bl, 0) <= 0) return;
        int v = (i == RAMP_STEPS) ? to : (from + (to - from) * i / RAMP_STEPS);
        if (!write_dbv(v)) return;
        if (v == to) break;
        nanosleep(&ts, nullptr);
    }
}

static void exit_hdr_mode(int boost_en, int boost_lvl, int peak_lvl) {
    int bl = pread_int(g_fd_bl, 0);
    g_sdr_dbv = clamp_sdr(g_sdr_dbv > 0 ? g_sdr_dbv : bl);
    int out_dbv = sdr_target_for(g_sdr_dbv, boost_en, boost_lvl, peak_lvl);
    write_dbv(out_dbv);
    if (bl > g_sdr_dbv) write_bl(g_sdr_dbv);
    g_active = 0;
    df_set_feature(10, 0);
    apply_df_profile(0, 0);
}

static void handle_sig(int) {
    g_stop = 1;
    pthread_cond_broadcast(&g_cond);
}

int main() {
    signal(SIGTERM, handle_sig);
    signal(SIGINT, handle_sig);
    signal(SIGHUP, handle_sig);

    g_fd_disp = open(DISP_PARAM_PATH, O_RDWR | O_CLOEXEC);
    if (g_fd_disp < 0) return 0;
    g_fd_bl  = open(BL_PATH, O_RDWR | O_CLOEXEC);
    g_fd_blp = open(BLP_PATH, O_RDONLY | O_CLOEXEC);

    // SurfaceFlinger::addHdrLayerInfoListener requires AID_SYSTEM (1000).
    // All three sysfs nodes are already open (and owned by system:system / 0644).
    if (getuid() == 0) {
        (void)setgid(1000);
        (void)setuid(1000);
    }

    void* handle = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!handle) return 1;
    g_wait_service  = reinterpret_cast<wait_service_fn>(dlsym(handle, "AServiceManager_waitForService"));
    g_check_service = reinterpret_cast<check_service_fn>(dlsym(handle, "AServiceManager_checkService"));
    g_set_threads   = reinterpret_cast<set_threads_fn>(dlsym(handle, "ABinderProcess_setThreadPoolMaxThreadCount"));
    g_start_pool    = reinterpret_cast<void_fn>(dlsym(handle, "ABinderProcess_startThreadPool"));
    if (!g_wait_service || !g_start_pool) return 1;

    g_sf_cls = AIBinder_Class_define(SF_DESC, noop_on_create, noop_on_destroy, noop_on_transact);
    g_df_cls = AIBinder_Class_define(DF_DESC, noop_on_create, noop_on_destroy, noop_on_transact);

    if (g_set_threads) g_set_threads(2);
    g_start_pool();

    pthread_mutex_lock(&g_lock);
    refresh_prefs_locked();
    pthread_mutex_unlock(&g_lock);

    // Start zero-CPU kernel futex watchers for user preferences
    static const char* kProps[] = {
        "persist.sys.oplus.hdr_peak_level",
        "persist.sys.oplus.sdr2hdr_enable",
        "persist.sys.oplus.sdr2hdr_level",
        "persist.sys.oplus.keyguard.showing",
    };
    for (const char* p : kProps) {
        pthread_t tid;
        if (pthread_create(&tid, nullptr, prop_watch_thread, const_cast<char*>(p)) == 0) {
            pthread_detach(tid);
        }
    }

    // Create and register IHdrLayerInfoListener with SurfaceFlingerAIDL
    AIBinder_Class* cls = AIBinder_Class_define(
            HDR_LISTENER_DESC, noop_on_create, noop_on_destroy, listener_on_transact);
    AIBinder* listener = AIBinder_new(cls, nullptr);

    while (!g_stop && !register_hdr_listener(listener)) {
        sleep(2);
    }

    // Initialize SDR DBV mirror if screen is currently on
    int bl0 = pread_int(g_fd_bl, 0);
    if (bl0 >= 2) {
        g_sdr_dbv = clamp_sdr(bl0);
        write_dbv(sdr_target_for(g_sdr_dbv, g_boost_en, g_boost_lvl, g_peak_level));
    }

    // Main event loop: blocks infinitely in pthread_cond_wait (0.000% CPU) when SDR & Boost OFF
    while (!g_stop) {
        pthread_mutex_lock(&g_lock);
        while (!g_stop && !g_dirty) {
            if (g_active || g_boost_en) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 250 * 1000 * 1000;
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }
                if (pthread_cond_timedwait(&g_cond, &g_lock, &ts) == ETIMEDOUT) break;
            } else {
                pthread_cond_wait(&g_cond, &g_lock);
            }
        }
        int layers    = g_hdr_layers;
        float ratio   = g_hdr_ratio;
        int peak_lvl  = g_peak_level;
        int peak_dbv  = g_peak_dbv;
        int boost_en  = g_boost_en;
        int boost_lvl = g_boost_lvl;
        bool kg       = g_keyguard;
        g_dirty = false;
        pthread_mutex_unlock(&g_lock);

        if (g_stop) break;

        int bl  = pread_int(g_fd_bl, 0);
        int blp = pread_int(g_fd_blp, 4);
        int on  = (blp == 0 && bl > 0) ? 1 : 0;
        int bl_sdr = clamp_sdr(bl);

        if (!on) {
            if (g_active) {
                exit_hdr_mode(boost_en, boost_lvl, peak_lvl);
            } else if (g_last_df_tier > 0) {
                apply_df_profile(0, 0);
            }
            continue;
        }

        if (layers > 0 && !kg) {
            if (!g_active) {
                if (bl_sdr >= 2) g_sdr_dbv = bl_sdr;
                int start = sdr_target_for(g_sdr_dbv, boost_en, boost_lvl, peak_lvl);
                if (g_last22 > start) start = g_last22;
                int tgt = peak_target_for(g_sdr_dbv, ratio, peak_lvl, peak_dbv);
                if (tgt < start) tgt = start;
                g_active = 1;
                apply_df_profile(compute_df_tier(1, 1, peak_lvl, boost_en, boost_lvl), 1);
                ramp_dbv(start, tgt);
                df_set_feature(10, 1);
            } else {
                if (bl_sdr >= 2) {
                    int diff = bl_sdr - g_sdr_dbv;
                    if (diff < -8 || diff > EDR_BUMP_MAX) g_sdr_dbv = bl_sdr;
                }
                apply_df_profile(compute_df_tier(1, 1, peak_lvl, boost_en, boost_lvl), 1);
                int tgt = peak_target_for(g_sdr_dbv, ratio, peak_lvl, peak_dbv);
                if (g_last22 > 0 && tgt != g_last22) {
                    int d = abs(tgt - g_last22);
                    int lim = g_last22 / 8;
                    if (lim < 24) lim = 24;
                    if (d > lim) write_dbv(tgt);
                }
            }
        } else {
            if (g_active) {
                exit_hdr_mode(boost_en, boost_lvl, peak_lvl);
            } else if (bl_sdr >= 2) {
                g_sdr_dbv = bl_sdr;
                int want = sdr_target_for(bl_sdr, boost_en, boost_lvl, peak_lvl);
                if (g_last22 < 0 || abs(want - g_last22) > 4) {
                    write_dbv(want);
                }
            }
        }
    }

    if (g_active) exit_hdr_mode(0, 0, 100);
    if (g_df_binder) AIBinder_decStrong(g_df_binder);
    if (listener) AIBinder_decStrong(listener);
    if (g_fd_disp >= 0) close(g_fd_disp);
    if (g_fd_bl >= 0) close(g_fd_bl);
    if (g_fd_blp >= 0) close(g_fd_blp);
    return 0;
}
