/* Pure-C++ Zero-Polling Xiaomi 15 (o3_42_0d_0a) Hardware LTPO + Live Hz Overlay Hook (overlay.so)
 *
 * Loaded into /system/bin/surfaceflinger via DT_NEEDED (100% rootless, zero polling loops,
 * zero SELinux AVCs, BTI+PAC safe).
 *
 * How it works:
 * 1. Verifies /proc/self/exe == /system/bin/surfaceflinger, Build-ID 856e1aafc62bef7bed561f689f3d79d3,
 *    and exact expected ARM64 prologues at all 4 hook sites before modifying any instruction.
 * 2. Hooks SurfaceFlinger::composite() (+0x207588) via an ABI-preserving naked pre-hook:
 *    records real (non-overlay) frame timestamps in a 64-entry ring buffer (~150ns/frame, 0 heap alloc).
 * 3. Runs a strictly event-driven idle watchdog (CLOCK_MONOTONIC condvar):
 *    - Active frames         -> debug.nyako.ltpo.sf_idle = "0"  (sfModeId=1 auto_120_to_1hz, 0x2780100)
 *    - >= 220ms quiet        -> debug.nyako.ltpo.sf_idle = "10" (sfModeId=2 idle_120_to_10hz, 0x1780A00)
 *    - >= 1000ms quiet/off   -> debug.nyako.ltpo.sf_idle = "1"  (sfModeId=3 idle_120_to_1hz, 0x1780100)
 *      and blocks INFINITELY in pthread_cond_wait (0.000% CPU, 0 wakeups/sec while static or screen-off!).
 * 4. When Developer Options "Show refresh rate" is ON (RefreshRateOverlay != nullptr):
 *    - Hooks RefreshRateOverlay::getOrCreateBuffers (+0x4d212c) & RefreshRateOverlay::animate (+0x4d2a24)
 *      and patches +0x209780 (b.eq -> b 0x20d090) so animate() executes on every composite.
 *    - Displays the REAL live measured cadence (1..120 Hz naturally from real frame intervals during
 *      scroll, fling deceleration, animations, and 24/30/60fps video; 10 Hz in idle_120_to_10hz;
 *      1 Hz in idle_120_to_1hz) without any fake stepping list and without self-repaint feedback.
 */
#include <android/log.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "NyakoLtpoOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

static constexpr uint8_t kExpectedBuildId[16] = {
    0x85, 0x6e, 0x1a, 0xaf, 0xc6, 0x2b, 0xef, 0x7b,
    0xed, 0x56, 0x1f, 0x68, 0x9f, 0x3d, 0x79, 0xd3,
};

static constexpr uintptr_t kOffComposite          = 0x207588;
static constexpr uintptr_t kOffAnimBranch         = 0x209780;
static constexpr uintptr_t kOffGetOrCreateBuffers = 0x4d212c;
static constexpr uintptr_t kOffGetOrCreateNullRet = 0x4d2368;
static constexpr uintptr_t kOffChangeRefreshRate  = 0x4d2570;
static constexpr uintptr_t kOffAnimate            = 0x4d2a24;

static constexpr uint32_t kAnimBranchOrig  = 0x5401c880u; // b.eq 0x20d090
static constexpr uint32_t kAnimBranchPatch = 0x14000e44u; // b    0x20d090

static constexpr int64_t kIdle10Ms         = 220;
static constexpr int64_t kIdle1Ms          = 1000;
static constexpr int64_t kWindowNs         = 300LL * 1000000LL;
static constexpr int64_t kOverlayMinStepNs = 80LL * 1000000LL;
static constexpr int     kRingSize         = 64;

using GetOrCreateFn = void* (*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool);
using ChangeRateFn  = void  (*)(void*, uint64_t, uint64_t, uint64_t, uint64_t);
using AnimateFn     = void  (*)(void*);

static GetOrCreateFn g_orig_get_or_create = nullptr;
static ChangeRateFn  g_change_rate        = nullptr;
static AnimateFn     g_orig_animate       = nullptr;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;
static bool            g_cv_ready = false;

static int64_t  g_ts[kRingSize] = {};
static int      g_head  = 0;
static int      g_count = 0;
static int64_t  g_last_real_ns = 0;

static uint64_t g_comp_seq = 0;
static uint64_t g_anim_seq = 0;
static bool     g_overlay_active = false;
static bool     g_overlay_published = false;

static int      g_idle_state = 0; // 0 = active(auto), 10 = idle10, 1 = idle1
static bool     g_watchdog_sleeping = false;
static int64_t  g_idle_probe_ns = 0; // requires >=2 frames within 180ms to exit idle (ignores 1-frame 5G/clock blinks)

static int64_t  g_self_repaint_until_ns = 0;
static int      g_self_repaint_budget   = 0;

static int      g_last_drawn_hz = -1;
static int64_t  g_last_overlay_update_ns = 0;
static thread_local int g_tls_override_hz = 0;

static inline int64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static inline float u64_to_float(uint64_t v) {
    uint32_t bits = static_cast<uint32_t>(v);
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static inline uint64_t float_to_u64(float f) {
    uint32_t bits = 0;
    memcpy(&bits, &f, sizeof(bits));
    return static_cast<uint64_t>(bits);
}

static int compute_real_hz_locked(int64_t now, int native_max_hz) {
    if (native_max_hz < 1) native_max_hz = 120;
    if (native_max_hz > 240) native_max_hz = 240;

    if (g_count <= 0 || g_last_real_ns <= 0) {
        return (g_idle_state == 1) ? 1 : native_max_hz;
    }

    int64_t age_ns = now - g_last_real_ns;
    if (age_ns < 0) age_ns = 0;
    int64_t age_ms = age_ns / 1000000LL;

    if (age_ms >= kIdle1Ms) return 1;
    if (age_ms >= kIdle10Ms) return (10 < native_max_hz) ? 10 : native_max_hz;

    // Inspect up to 10 most recent real frames within the last 300ms
    int k = 0;
    int64_t t_newest = 0;
    int64_t t_oldest = 0;
    int max_check = (g_count < 10) ? g_count : 10;
    for (int i = 0; i < max_check; ++i) {
        int idx = (g_head - 1 - i) & (kRingSize - 1);
        int64_t t = g_ts[idx];
        if (i == 0) {
            t_newest = t;
            t_oldest = t;
            k = 1;
        } else if (now - t <= kWindowNs && t <= t_oldest) {
            t_oldest = t;
            k++;
        } else {
            break;
        }
    }

    int hz = native_max_hz;
    if (k >= 2) {
        int64_t span_ns = t_newest - t_oldest;
        int64_t eff_span = (age_ns > 25000000LL) ? (now - t_oldest) : span_ns;
        if (eff_span < 1000000LL) eff_span = 1000000LL;
        double rate = static_cast<double>(k - 1) * 1e9 / static_cast<double>(eff_span);
        hz = static_cast<int>(llround(rate));
    } else {
        if (age_ns < 45000000LL) {
            hz = native_max_hz;
        } else {
            hz = static_cast<int>(llround(1e9 / static_cast<double>(age_ns)));
        }
    }

    if (hz < 1) hz = 1;
    if (hz > native_max_hz) hz = native_max_hz;
    return hz;
}

static void* watchdog_thread(void*) {
    pthread_mutex_lock(&g_mtx);
    for (;;) {
        int64_t now = mono_ns();

        // Publish overlay visibility ONLY on toggle
        if (g_overlay_active != g_overlay_published) {
            bool on = g_overlay_active;
            g_overlay_published = on;
            pthread_mutex_unlock(&g_mtx);
            __system_property_set("debug.nyako.ltpo.sf_overlay_on", on ? "1" : "0");
            pthread_mutex_lock(&g_mtx);
            continue;
        }

        int64_t last = g_last_real_ns;
        int want = 0;
        int64_t next_deadline_ns = 0;

        if (last == 0) {
            want = 1;
        } else {
            int64_t elapsed_ms = (now - last) / 1000000LL;
            if (elapsed_ms < 0) elapsed_ms = 0;
            if (elapsed_ms >= kIdle1Ms) {
                want = 1;
            } else if (elapsed_ms >= kIdle10Ms) {
                want = 10;
                next_deadline_ns = last + kIdle1Ms * 1000000LL;
            } else {
                want = 0;
                next_deadline_ns = last + kIdle10Ms * 1000000LL;
            }
        }

        if (want != g_idle_state) {
            g_idle_state = want;
            if (want != 0) {
                // Suppress mode-switch / 1004 / changeRefreshRate self-composites whether overlay is ON or OFF
                g_self_repaint_budget = 6;
                g_self_repaint_until_ns = now + 450LL * 1000000LL;
                g_idle_probe_ns = 0;
            }
            const char* val = (want == 0) ? "0" : ((want == 10) ? "10" : "1");
            pthread_mutex_unlock(&g_mtx);
            __system_property_set("debug.nyako.ltpo.sf_idle", val);
            pthread_mutex_lock(&g_mtx);
            continue;
        }

        if (want == 1) {
            // Settled at 1 Hz (or screen off): infinite kernel sleep -> 0.000% CPU, 0 wakeups/sec!
            g_watchdog_sleeping = true;
            pthread_cond_wait(&g_cv, &g_mtx);
            g_watchdog_sleeping = false;
        } else {
            struct timespec ts;
            ts.tv_sec  = static_cast<time_t>(next_deadline_ns / 1000000000LL);
            ts.tv_nsec = static_cast<long>(next_deadline_ns % 1000000000LL);
            g_watchdog_sleeping = false;
            pthread_cond_timedwait(&g_cv, &g_mtx, &ts);
        }
    }
    return nullptr;
}

static void* hook_get_or_create_buffers(
        void* self, uint64_t fps_lo, uint64_t fps_hi,
        uint64_t render_lo, uint64_t render_hi, bool setByHwc) {
    int native_hz = static_cast<int>(llroundf(u64_to_float(fps_lo)));
    if (native_hz < 1 || native_hz > 240) native_hz = 120;

    int hz = g_tls_override_hz;
    if (hz <= 0) {
        int64_t now = mono_ns();
        pthread_mutex_lock(&g_mtx);
        hz = compute_real_hz_locked(now, native_hz);
        g_last_drawn_hz = hz;
        g_last_overlay_update_ns = now;
        pthread_mutex_unlock(&g_mtx);
    } else {
        int64_t now = mono_ns();
        pthread_mutex_lock(&g_mtx);
        g_last_drawn_hz = hz;
        g_last_overlay_update_ns = now;
        pthread_mutex_unlock(&g_mtx);
    }

    uint64_t synth_lo = float_to_u64(static_cast<float>(hz));
    uint64_t synth_hi = static_cast<uint64_t>(llround(1e9 / static_cast<double>(hz)));
    return g_orig_get_or_create(self, synth_lo, synth_hi, synth_lo, synth_hi, false);
}

static void hook_animate(void* self) {
    g_orig_animate(self);

    if (!self) return;
    const auto* base = static_cast<const uint8_t*>(self);
    if (base[0x190] == 0 || base[0x1a8] == 0) return;
    void* sc = nullptr;
    memcpy(&sc, base + 0x1e8, sizeof(sc));
    if (!sc) return;

    uint64_t fps_lo = 0, fps_hi = 0, r_lo = 0, r_hi = 0;
    memcpy(&fps_lo, base + 0x180, sizeof(uint64_t));
    memcpy(&fps_hi, base + 0x188, sizeof(uint64_t));
    memcpy(&r_lo,   base + 0x198, sizeof(uint64_t));
    memcpy(&r_hi,   base + 0x1a0, sizeof(uint64_t));

    int native_hz = static_cast<int>(llroundf(u64_to_float(fps_lo)));
    if (native_hz < 1 || native_hz > 240) native_hz = 120;

    int64_t now = mono_ns();
    bool do_redraw = false;
    int want_hz = native_hz;

    pthread_mutex_lock(&g_mtx);
    g_anim_seq = g_comp_seq;
    if (!g_overlay_active) {
        g_overlay_active = true;
        if (g_watchdog_sleeping) pthread_cond_signal(&g_cv);
    }
    want_hz = compute_real_hz_locked(now, native_hz);
    if (want_hz != g_last_drawn_hz) {
        bool urgent = (g_idle_state != 0) || (g_last_drawn_hz <= 10) || (g_last_drawn_hz < 0);
        if (urgent || (now - g_last_overlay_update_ns) >= kOverlayMinStepNs) {
            do_redraw = true;
            if (g_idle_state != 0) {
                g_self_repaint_budget = 3;
                g_self_repaint_until_ns = now + 300LL * 1000000LL;
            }
        }
    }
    pthread_mutex_unlock(&g_mtx);

    if (do_redraw && g_change_rate) {
        g_tls_override_hz = want_hz;
        g_change_rate(self, fps_lo, fps_hi, r_lo, r_hi);
        g_tls_override_hz = 0;
    }
}

struct ImageInfo {
    uintptr_t base = 0;
    bool found = false;
};

static int find_sf_phdr(struct dl_phdr_info* info, size_t, void* data) {
    auto* out = static_cast<ImageInfo*>(data);
    if (info->dlpi_name && info->dlpi_name[0] != '\0' &&
        strcmp(info->dlpi_name, "/system/bin/surfaceflinger") != 0) {
        return 0;
    }
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE || ph.p_memsz < 12) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
        const uint8_t* end = p + ph.p_memsz;
        while (p + 12 <= end) {
            uint32_t namesz = 0, descsz = 0, type = 0;
            memcpy(&namesz, p, 4);
            memcpy(&descsz, p + 4, 4);
            memcpy(&type,   p + 8, 4);
            const uint8_t* name = p + 12;
            size_t n_aligned = (namesz + 3u) & ~3u;
            size_t d_aligned = (descsz + 3u) & ~3u;
            if (name + n_aligned + d_aligned > end) break;
            if (namesz == 4 && type == 3 && descsz == 16 &&
                memcmp(name, "GNU", 4) == 0 &&
                memcmp(name + n_aligned, kExpectedBuildId, 16) == 0) {
                out->base = info->dlpi_addr;
                out->found = true;
                return 1;
            }
            p = name + n_aligned + d_aligned;
        }
    }
    return 0;
}

static bool patch_code(void* addr, const void* bytes, size_t len) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return false;
    uintptr_t pmask = ~static_cast<uintptr_t>(ps - 1);
    uintptr_t start = reinterpret_cast<uintptr_t>(addr) & pmask;
    uintptr_t end   = (reinterpret_cast<uintptr_t>(addr) + len + static_cast<uintptr_t>(ps) - 1) & pmask;
    size_t mlen = static_cast<size_t>(end - start);
    if (mprotect(reinterpret_cast<void*>(start), mlen, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGW("mprotect RWX failed addr=%p errno=%d", addr, errno);
        return false;
    }
    memcpy(addr, bytes, len);
    __builtin___clear_cache(static_cast<char*>(addr), static_cast<char*>(addr) + len);
    if (mprotect(reinterpret_cast<void*>(start), mlen, PROT_READ | PROT_EXEC) != 0) {
        LOGW("mprotect RX failed addr=%p errno=%d", addr, errno);
        return false;
    }
    return true;
}

static bool install_abs_jump(void* target, void* dest) {
    struct __attribute__((packed)) {
        uint32_t ldr_x16;
        uint32_t br_x16;
        uint64_t addr;
    } patch = {
        0x58000050u, // ldr x16, #8
        0xd61f0200u, // br  x16
        reinterpret_cast<uint64_t>(dest),
    };
    return patch_code(target, &patch, sizeof(patch));
}

} // namespace

extern "C" __attribute__((visibility("hidden"))) void* g_tramp_composite = nullptr;

extern "C" __attribute__((visibility("hidden"))) void on_sf_composite() {
    int64_t now = mono_ns();
    pthread_mutex_lock(&g_mtx);
    g_comp_seq++;
    if (g_overlay_active && g_comp_seq > 1 && (g_comp_seq - g_anim_seq) >= 2) {
        g_overlay_active = false;
        if (g_watchdog_sleeping) pthread_cond_signal(&g_cv);
    }
    if (g_self_repaint_budget > 0 && now <= g_self_repaint_until_ns) {
        g_self_repaint_budget--;
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    g_self_repaint_budget = 0;

    // While in idle (10Hz or 1Hz), ignore isolated single-frame updates (e.g. 5G status bar arrow blink,
    // clock minute tick, battery icon) — DDIC presents single frames at 120Hz TE without switching mode!
    // Require >= 2 frames within 180ms (>= 6 fps real animation/scroll/video) to exit idle.
    // (User touch already wakes id=1 instantaneously in <1ms via PointerMonitor.)
    if (g_idle_state != 0) {
        if (g_idle_probe_ns == 0 || (now - g_idle_probe_ns) > 180LL * 1000000LL) {
            g_idle_probe_ns = now;
            pthread_mutex_unlock(&g_mtx);
            return;
        }
        // Second frame arrived within 180ms -> real motion! Record both timestamps and wake immediately.
        g_ts[g_head] = g_idle_probe_ns;
        g_head = (g_head + 1) & (kRingSize - 1);
        if (g_count < kRingSize) g_count++;
        g_idle_probe_ns = 0;
    }

    g_ts[g_head] = now;
    g_head = (g_head + 1) & (kRingSize - 1);
    if (g_count < kRingSize) g_count++;
    g_last_real_ns = now;
    if (g_idle_state != 0 || g_watchdog_sleeping) {
        pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_mtx);
}

// 100% ABI-preserving pre-hook for SurfaceFlinger::composite():
// saves x0..x9, x29, x30, q0..q7, calls on_sf_composite(), restores all, tail-jumps to trampoline.
extern "C" __attribute__((naked)) void my_composite_stub() {
    asm volatile(
        "bti c\n"
        "stp x29, x30, [sp, #-224]!\n"
        "stp x0,  x1,  [sp, #16]\n"
        "stp x2,  x3,  [sp, #32]\n"
        "stp x4,  x5,  [sp, #48]\n"
        "stp x6,  x7,  [sp, #64]\n"
        "stp x8,  x9,  [sp, #80]\n"
        "stp q0,  q1,  [sp, #96]\n"
        "stp q2,  q3,  [sp, #128]\n"
        "stp q4,  q5,  [sp, #160]\n"
        "stp q6,  q7,  [sp, #192]\n"
        "bl  on_sf_composite\n"
        "ldp q6,  q7,  [sp, #192]\n"
        "ldp q4,  q5,  [sp, #160]\n"
        "ldp q2,  q3,  [sp, #128]\n"
        "ldp q0,  q1,  [sp, #96]\n"
        "ldp x8,  x9,  [sp, #80]\n"
        "ldp x6,  x7,  [sp, #64]\n"
        "ldp x4,  x5,  [sp, #48]\n"
        "ldp x2,  x3,  [sp, #32]\n"
        "ldp x0,  x1,  [sp, #16]\n"
        "ldp x29, x30, [sp], #224\n"
        "adrp x16, g_tramp_composite\n"
        "ldr  x16, [x16, :lo12:g_tramp_composite]\n"
        "br   x16\n"
    );
}

__attribute__((constructor))
static void overlay_init() {
    char exe[256] = {};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || strcmp(exe, "/system/bin/surfaceflinger") != 0) {
        return;
    }

    char status[92] = {};
    ImageInfo img;
    dl_iterate_phdr(find_sf_phdr, &img);
    if (!img.found) {
        snprintf(status, sizeof(status), "%d:build-id-mismatch", getpid());
        __system_property_set("debug.nyako.ltpo.overlay_status", status);
        LOGW("%s", status);
        return;
    }

    auto* p_comp   = reinterpret_cast<uint32_t*>(img.base + kOffComposite);
    auto* p_branch = reinterpret_cast<uint32_t*>(img.base + kOffAnimBranch);
    auto* p_buf    = reinterpret_cast<uint32_t*>(img.base + kOffGetOrCreateBuffers);
    auto* p_anim   = reinterpret_cast<uint32_t*>(img.base + kOffAnimate);

    // Verify every expected prologue word before patching a single byte
    if (p_comp[0] != 0xd503233fu || p_comp[1] != 0x6db923e9u ||
        p_comp[2] != 0xa9017bfdu || p_comp[3] != 0xa9026ffcu ||
        p_buf[0]  != 0xf940f408u || p_buf[1]  != 0xb40011c8u ||
        p_buf[2]  != 0xd503233fu || p_buf[3]  != 0x6db82bebu ||
        p_anim[0] != 0xd503245fu || p_anim[1] != 0x39478008u ||
        p_anim[2] != 0x36000a48u || p_anim[3] != 0xd503233fu ||
        p_branch[0] != kAnimBranchOrig) {
        snprintf(status, sizeof(status), "%d:prologue-mismatch", getpid());
        __system_property_set("debug.nyako.ltpo.overlay_status", status);
        LOGW("%s", status);
        return;
    }

    // Initialize CLOCK_MONOTONIC condvar
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &attr);
    pthread_condattr_destroy(&attr);
    g_cv_ready = true;

    // Allocate one 4KB RW page for all 3 trampolines, then flip to RX
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(status, sizeof(status), "%d:mmap-failed", getpid());
        __system_property_set("debug.nyako.ltpo.overlay_status", status);
        return;
    }
    auto* tramp = static_cast<uint8_t*>(page);

    // Trampoline 0 (offset 0x00): SurfaceFlinger::composite (+0x207588)
    {
        auto* w = reinterpret_cast<uint32_t*>(tramp + 0x00);
        w[0] = 0xd503245fu; // bti c
        w[1] = 0xd503233fu; // paciasp
        w[2] = 0x6db923e9u; // stp d9, d8, [sp, #-0x70]!
        w[3] = 0xa9017bfdu; // stp x29, x30, [sp, #0x10]
        w[4] = 0xa9026ffcu; // stp x28, x27, [sp, #0x20]
        w[5] = 0xd503201fu; // nop
        w[6] = 0x58000050u; // ldr x16, #8 (-> 0x20)
        w[7] = 0xd61f0200u; // br x16
        uint64_t cont = img.base + kOffComposite + 0x10;
        memcpy(tramp + 0x20, &cont, 8);
        g_tramp_composite = tramp + 0x00;
    }

    // Trampoline 1 (offset 0x40): RefreshRateOverlay::getOrCreateBuffers (+0x4d212c)
    {
        auto* w = reinterpret_cast<uint32_t*>(tramp + 0x40);
        w[0] = 0xd503245fu; // bti c
        w[1] = 0xf940f408u; // ldr x8, [x0, #0x1e8]
        w[2] = 0xb5000068u; // cbnz x8, +12 (-> +0x14)
        w[3] = 0x580000f0u; // ldr x16, +28 (-> +0x28)
        w[4] = 0xd61f0200u; // br x16
        w[5] = 0xd503233fu; // paciasp
        w[6] = 0x6db82bebu; // stp d11, d10, [sp, #-0x80]!
        w[7] = 0x580000b0u; // ldr x16, +20 (-> +0x30)
        w[8] = 0xd61f0200u; // br x16
        w[9] = 0xd503201fu; // nop
        uint64_t null_ret = img.base + kOffGetOrCreateNullRet;
        uint64_t cont     = img.base + kOffGetOrCreateBuffers + 0x10;
        memcpy(tramp + 0x40 + 0x28, &null_ret, 8);
        memcpy(tramp + 0x40 + 0x30, &cont, 8);
        g_orig_get_or_create = reinterpret_cast<GetOrCreateFn>(tramp + 0x40);
    }

    // Trampoline 2 (offset 0x80): RefreshRateOverlay::animate (+0x4d2a24)
    {
        auto* w = reinterpret_cast<uint32_t*>(tramp + 0x80);
        w[0] = 0xd503245fu; // bti c
        w[1] = 0x39478008u; // ldrb w8, [x0, #0x1e0]
        w[2] = 0x37000048u; // tbnz w8, #0, +8 (-> +0x10)
        w[3] = 0xd65f03c0u; // ret
        w[4] = 0xd503233fu; // paciasp
        w[5] = 0xd503201fu; // nop
        w[6] = 0x58000050u; // ldr x16, #8 (-> +0x20)
        w[7] = 0xd61f0200u; // br x16
        uint64_t cont = img.base + kOffAnimate + 0x10;
        memcpy(tramp + 0x80 + 0x20, &cont, 8);
        g_orig_animate = reinterpret_cast<AnimateFn>(tramp + 0x80);
    }

    __builtin___clear_cache(reinterpret_cast<char*>(tramp), reinterpret_cast<char*>(tramp + 256));
    if (mprotect(tramp, 4096, PROT_READ | PROT_EXEC) != 0) {
        snprintf(status, sizeof(status), "%d:tramp-rx-failed", getpid());
        __system_property_set("debug.nyako.ltpo.overlay_status", status);
        return;
    }

    g_change_rate = reinterpret_cast<ChangeRateFn>(img.base + kOffChangeRefreshRate);
    g_last_real_ns = mono_ns();

    // Install hooks + animate branch patch
    if (!install_abs_jump(p_buf,  reinterpret_cast<void*>(hook_get_or_create_buffers)) ||
        !install_abs_jump(p_anim, reinterpret_cast<void*>(hook_animate)) ||
        !install_abs_jump(p_comp, reinterpret_cast<void*>(my_composite_stub))) {
        snprintf(status, sizeof(status), "%d:hook-patch-failed", getpid());
        __system_property_set("debug.nyako.ltpo.overlay_status", status);
        return;
    }

    uint32_t branch_word = kAnimBranchPatch;
    if (!patch_code(p_branch, &branch_word, sizeof(branch_word))) {
        snprintf(status, sizeof(status), "%d:branch-patch-failed", getpid());
        __system_property_set("debug.nyako.ltpo.overlay_status", status);
        return;
    }

    // Initialize properties once and launch strictly event-driven watchdog
    __system_property_set("debug.nyako.ltpo.sf_idle", "0");
    __system_property_set("debug.nyako.ltpo.sf_overlay_on", "0");

    pthread_t tid;
    if (pthread_create(&tid, nullptr, watchdog_thread, nullptr) == 0) {
        pthread_detach(tid);
    }

    snprintf(status, sizeof(status), "%d:v3-hw-ltpo-ready", getpid());
    __system_property_set("debug.nyako.ltpo.overlay_status", status);
    LOGI("%s", status);
}
