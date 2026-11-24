// xmvibproxy_v2.cpp — Clean C++17 Vibrator Proxy for Xiaomi 15 (dada) ColorOS 17
// 1) Wraps IVibrator/default & IVibratorManager/default with /vendor/etc/xmvibproxy.conf ringtone support
//    (ZERO /data/local/tmp probes -> 0 SELinux AVCs).
// 2) Forwards IOplusVibrator extension via AIBinder_getExtension -> AIBinder_setExtension
//    (fixes E OplusVibratorManager: getExtension failed).
// 3) Implements native multi-texture AAC RichTap HE 1.0 & HE 2.0 playback inside IOplusVibrator proxy
//    using Xiaomi 15's 203 hardware RTP waveforms (/odm/firmware/*_P_RTP.bin) + pre-upload amplitude scaling
//    on a dedicated condvar-driven worker thread (ZERO polling loops, 0.0% idle CPU).

#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#define TAG "XmVibProxy"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct AIBinder;
struct AIBinder_Class;
struct AIBinder_DeathRecipient;
struct AParcel;

using binder_status_t = int32_t;
constexpr uint32_t FLAG_ONEWAY = 0x01;

// Dynamically resolved libbinder_ndk symbols (exact vendor ABI parity)
struct NdkBinder {
    AIBinder_Class* (*Class_define)(const char*, void* (*)(void*), void (*)(void*),
                                    binder_status_t (*)(AIBinder*, uint32_t, const AParcel*, AParcel*));
    AIBinder* (*new_binder)(const AIBinder_Class*, void*);
    void* (*getUserData)(AIBinder*);
    void (*incStrong)(AIBinder*);
    void (*decStrong)(AIBinder*);
    bool (*isRemote)(const AIBinder*);
    bool (*associateClass)(AIBinder*, const AIBinder_Class*);
    bool (*lt)(const AIBinder*, const AIBinder*);
    void (*markVintfStability)(AIBinder*);
    binder_status_t (*getExtension)(AIBinder*, AIBinder**);
    binder_status_t (*setExtension)(AIBinder*, AIBinder*);
    binder_status_t (*prepareTransaction)(AIBinder*, AParcel**);
    binder_status_t (*transact)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t);
    AIBinder_DeathRecipient* (*DeathRecipient_new)(void (*)(void*));
    binder_status_t (*linkToDeath)(AIBinder*, AIBinder_DeathRecipient*, void*);

    void (*AParcel_delete)(AParcel*);
    int32_t (*AParcel_getDataPosition)(const AParcel*);
    binder_status_t (*AParcel_setDataPosition)(const AParcel*, int32_t);
    int32_t (*AParcel_getDataSize)(const AParcel*);
    binder_status_t (*AParcel_appendFrom)(const AParcel*, AParcel*, int32_t, int32_t);
    binder_status_t (*AParcel_readInt32)(const AParcel*, int32_t*);
    binder_status_t (*AParcel_writeInt32)(AParcel*, int32_t);
    binder_status_t (*AParcel_writeFloat)(AParcel*, float);
    binder_status_t (*AParcel_readStrongBinder)(const AParcel*, AIBinder**);
    binder_status_t (*AParcel_writeStrongBinder)(AParcel*, AIBinder*);
    binder_status_t (*AParcel_readInt32Array)(const AParcel*, void*, bool (*)(void*, int32_t, int32_t**));

    AIBinder* (*checkService)(const char*);
    AIBinder* (*waitForService)(const char*);
    binder_status_t (*addService)(AIBinder*, const char*);
    void (*setThreadPoolMaxThreadCount)(uint32_t);
    void (*startThreadPool)();
    void (*joinThreadPool)();
};

static NdkBinder g_ndk{};
static AIBinder_Class* g_cls_vib = nullptr;
static AIBinder_Class* g_cls_mgr = nullptr;
static AIBinder_Class* g_cls_ext = nullptr;
static AIBinder_Class* g_cls_cb = nullptr;
static AIBinder_Class* g_cls_rich_cb = nullptr;
static AIBinder* g_real_mgr = nullptr;

// Lazy connection to Xiaomi vibratorfeature HAL for direct hardware RTP texture playback
static pthread_mutex_t g_xm_mu = PTHREAD_MUTEX_INITIALIZER;
static AIBinder* g_xm_vib = nullptr;

static AIBinder* get_xm_vib_locked() {
    if (!g_xm_vib) {
        g_xm_vib = g_ndk.checkService("android.hardware.vibrator.IVibrator/vibratorfeature");
        if (g_xm_vib) {
            g_ndk.associateClass(g_xm_vib, g_cls_vib);
            LOGI("connected IVibrator/vibratorfeature=%p", g_xm_vib);
        }
    }
    return g_xm_vib;
}

static bool xm_set_amplitude_and_perform(int32_t effect_id, int8_t strength, float amp) {
    pthread_mutex_lock(&g_xm_mu);
    AIBinder* vib = get_xm_vib_locked();
    if (!vib) {
        pthread_mutex_unlock(&g_xm_mu);
        return false;
    }
    if (amp < 0.05f) amp = 0.05f;
    if (amp > 1.0f) amp = 1.0f;

    // 1) Pre-upload amplitude via IVibrator::setAmplitude (code 6) -> stores to vibhal BSS [0x3c01c]
    {
        AParcel *in = nullptr, *out = nullptr;
        if (g_ndk.prepareTransaction(vib, &in) == 0) {
            g_ndk.AParcel_writeFloat(in, amp);
            g_ndk.transact(vib, 6, &in, &out, 0);
            if (out) g_ndk.AParcel_delete(out);
        }
    }
    // 2) IVibrator::perform (code 4): int32 effect, int32/byte strength, IBinder callback (null)
    bool ok = false;
    {
        AParcel *in = nullptr, *out = nullptr;
        if (g_ndk.prepareTransaction(vib, &in) == 0) {
            g_ndk.AParcel_writeInt32(in, effect_id);
            g_ndk.AParcel_writeInt32(in, (int32_t)strength);
            g_ndk.AParcel_writeStrongBinder(in, nullptr);
            binder_status_t st = g_ndk.transact(vib, 4, &in, &out, 0);
            if (st == 0 && out) {
                int32_t exc = -1;
                g_ndk.AParcel_readInt32(out, &exc);
                ok = (exc == 0);
            }
            if (out) g_ndk.AParcel_delete(out);
        }
    }
    pthread_mutex_unlock(&g_xm_mu);
    return ok;
}

static void xm_off() {
    pthread_mutex_lock(&g_xm_mu);
    AIBinder* vib = get_xm_vib_locked();
    if (vib) {
        AParcel *in = nullptr, *out = nullptr;
        if (g_ndk.prepareTransaction(vib, &in) == 0) {
            g_ndk.transact(vib, 2, &in, &out, 0);
            if (out) g_ndk.AParcel_delete(out);
        }
    }
    pthread_mutex_unlock(&g_xm_mu);
}

static void fire_richtap_callback(AIBinder* cb) {
    if (!cb) return;
    if (g_ndk.associateClass(cb, g_cls_rich_cb)) {
        AParcel *in = nullptr, *out = nullptr;
        if (g_ndk.prepareTransaction(cb, &in) == 0) {
            g_ndk.AParcel_writeInt32(in, 0);
            g_ndk.transact(cb, 1, &in, &out, FLAG_ONEWAY);
            if (out) g_ndk.AParcel_delete(out);
        }
    }
}

static void fire_vib_callback(AIBinder* cb) {
    if (!cb) return;
    if (g_ndk.associateClass(cb, g_cls_cb)) {
        AParcel *in = nullptr, *out = nullptr;
        if (g_ndk.prepareTransaction(cb, &in) == 0) {
            g_ndk.transact(cb, 1, &in, &out, FLAG_ONEWAY);
            if (out) g_ndk.AParcel_delete(out);
        }
    }
}

// ============================================================================
// Ringtone Envelope Table (/vendor/etc/xmvibproxy.conf) — 100% stock parity
// ============================================================================
struct RingSeg { int32_t ms; int32_t amp; };
struct RingPattern {
    int32_t id = -1;
    bool loop = false;
    int32_t total_ms = 0;
    std::vector<RingSeg> segs;
};
static std::vector<RingPattern> g_ring_patterns;
static int g_ring_mode = 0;

static void load_ring_conf() {
    FILE* f = fopen("/vendor/etc/xmvibproxy.conf", "r");
    if (!f) {
        LOGW("conf /vendor/etc/xmvibproxy.conf not found");
        return;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        if (strncmp(p, "@mode", 5) == 0) {
            sscanf(p + 5, "%d", &g_ring_mode);
            continue;
        }
        int id = -1, n = 0;
        char kind[16] = {};
        if (sscanf(p, "%d %15s %n", &id, kind, &n) < 2) continue;
        RingPattern pat;
        pat.id = id;
        pat.loop = (strcmp(kind, "loop") == 0);
        p += n;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\n' || *p == '\r') break;
            int ms = 0, amp = 255, consumed = 0;
            if (sscanf(p, "%d:%d%n", &ms, &amp, &consumed) == 2 && consumed > 0) {
                pat.segs.push_back({ms, amp});
                pat.total_ms += ms;
                p += consumed;
            } else if (sscanf(p, "%d%n", &ms, &consumed) == 1 && consumed > 0) {
                // Alternating on/off format fallback
                int is_on = (pat.segs.size() % 2 == 0) ? 255 : 0;
                pat.segs.push_back({ms, is_on});
                pat.total_ms += ms;
                p += consumed;
            } else {
                break;
            }
        }
        if (!pat.segs.empty()) g_ring_patterns.push_back(std::move(pat));
    }
    fclose(f);
    LOGI("conf /vendor/etc/xmvibproxy.conf loaded: patterns=%zu mode=%d", g_ring_patterns.size(), g_ring_mode);
}

static const RingPattern* find_ring_pattern(int32_t id) {
    for (const auto& p : g_ring_patterns) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

// ============================================================================
// Dedicated Event-Driven Playback Worker (handles BOTH HE 1.0/2.0 AND Ringtone)
// ZERO polling loops — blocks on pthread_cond_wait when idle (0.0% CPU).
// ============================================================================
struct HeEvent {
    int32_t rel_ms;
    int32_t type;       // 0x1001 = transient, 0x1000 = continuous
    int32_t duration_ms;
    int32_t intensity;  // 0..100
    int32_t freq;       // 0..100
};

struct PlayJob {
    enum Kind { NONE, HE, RINGTONE } kind = NONE;
    uint64_t gen = 0;
    // HE fields
    std::vector<HeEvent> he_events;
    int32_t he_looper = 1;
    int32_t he_interval_ms = 0;
    float he_amp_scale = 1.0f;
    AIBinder* he_cb = nullptr;
    // Ringtone fields
    const RingPattern* ring_pat = nullptr;
    AIBinder* ring_real_vib = nullptr;
    AIBinder* ring_cb = nullptr;
};

static pthread_mutex_t g_work_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cv = PTHREAD_COND_INITIALIZER;
static std::atomic<uint64_t> g_play_gen{0};
static std::atomic<float> g_live_he_amp_scale{1.0f};
static PlayJob g_pending_job;
static bool g_has_job = false;

static void cancel_active_playback() {
    pthread_mutex_lock(&g_work_mu);
    g_play_gen.fetch_add(1, std::memory_order_acq_rel);
    if (g_has_job) {
        if (g_pending_job.he_cb) { g_ndk.decStrong(g_pending_job.he_cb); g_pending_job.he_cb = nullptr; }
        if (g_pending_job.ring_real_vib) { g_ndk.decStrong(g_pending_job.ring_real_vib); g_pending_job.ring_real_vib = nullptr; }
        if (g_pending_job.ring_cb) { g_ndk.decStrong(g_pending_job.ring_cb); g_pending_job.ring_cb = nullptr; }
        g_has_job = false;
    }
    pthread_cond_broadcast(&g_work_cv);
    pthread_mutex_unlock(&g_work_mu);
}

static bool sleep_cancellable_ms(uint64_t my_gen, int32_t ms) {
    if (ms <= 0) return (g_play_gen.load(std::memory_order_acquire) == my_gen);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ns = (int64_t)ts.tv_nsec + (int64_t)ms * 1000000LL;
    ts.tv_sec += ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;

    pthread_mutex_lock(&g_work_mu);
    while (g_play_gen.load(std::memory_order_acquire) == my_gen) {
        int rc = pthread_cond_timedwait(&g_work_cv, &g_work_mu, &ts);
        if (rc == ETIMEDOUT) break;
    }
    bool alive = (g_play_gen.load(std::memory_order_acquire) == my_gen);
    pthread_mutex_unlock(&g_work_mu);
    return alive;
}

// Select distinct instant-onset (<=2.9ms) Xiaomi 15 hardware RTP waveform for each HE event.
// Every target ID is PCM-verified at 24kHz:
//   - Transient (0x1001): crisp single-shot textures (12..25ms, Onset<=2.1ms)
//   - Continuous (0x1000): smooth single-envelope waveforms (bursts<=1, Onset<=2.9ms) so Setup Wizard
//     & continuous fades NEVER explode into multi-burst chatter (replaces 8-burst 205/171 & 2-burst 164).
static int32_t select_xiaomi_rtp_for_he(const HeEvent& ev) {
    if (ev.type == 0x1001) {
        // Transient event: map frequency & intensity to distinct crisp tactile textures (all <=25ms, Onset<=2.1ms)
        if (ev.freq >= 78) return (ev.intensity < 45) ? 178 : 181;     // 178_signalClockUnit (12ms) / 181_signalKeyUnit (13ms)
        if (ev.freq >= 60) return (ev.intensity >= 70) ? 4 : 201;      // 4_pop (16ms, 412Hz) / 201_BoundaryBounce (13ms, 301Hz)
        if (ev.freq >= 42) return (ev.intensity < 65) ? 166 : 2;       // 166_feedbackNeutral (14ms) / 2_tick (22ms)
        if (ev.freq >= 25) return (ev.intensity < 65) ? 188 : 163;     // 188_signalTab (25ms) / 163_GestureBackRelease (23ms)
        return (ev.intensity < 75) ? 88 : 202;                         // 88_launcherIconSelection (15ms, 98Hz deep) / 202_BoundaryBottom (22ms)
    } else {
        // Continuous event (0x1000): smooth single-envelope waveforms ONLY (zero multi-burst chatter!)
        if (ev.duration_ms >= 240) {
            return (ev.freq >= 50) ? 77 : 57;                          // 77_fodMotion1 (300ms, 1 env) / 57_FadeIn (215ms, 0 bursts)
        }
        if (ev.duration_ms >= 130) {
            return (ev.freq >= 55) ? 401 : 109;                        // 401_micon (164ms, 1 env) / 109_scopeOff (140ms, 1 env)
        }
        if (ev.duration_ms >= 60) {
            return (ev.freq >= 55) ? 85 : 106;                         // 85_screenshot (84ms, 1 env) / 106_sceneStep (68ms, 1 env)
        }
        return (ev.freq >= 50) ? 162 : 210;                            // 162_GestureBackPull (30ms) / 210_FingerprintUnlock (26ms)
    }
}

static void run_he_job(const PlayJob& job) {
    int loops = (job.he_looper <= 0) ? 1 : (job.he_looper > 16 ? 16 : job.he_looper);
    for (int l = 0; l < loops; ++l) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int32_t last_fire_ms = -100;
        int32_t last_ev_dur_ms = 48;

        for (size_t i = 0; i < job.he_events.size(); ++i) {
            const auto& ev = job.he_events[i];
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int32_t elapsed = (int32_t)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
            int32_t wait_ms = ev.rel_ms - elapsed;
            if (wait_ms > 0) {
                if (!sleep_cancellable_ms(job.gen, wait_ms)) {
                    xm_off();
                    return;
                }
            }
            // Re-check cancellation right before firing to eliminate trailing event after code=6/7/14/off
            if (g_play_gen.load(std::memory_order_acquire) != job.gen) {
                xm_off();
                return;
            }
            // Anti-collision spacing: >=45ms for continuous sweeps, >=18ms for crisp transients
            int32_t min_gap = (ev.type == 0x1000) ? 45 : 18;
            if (ev.rel_ms - last_fire_ms < min_gap && i > 0) continue;
            last_fire_ms = ev.rel_ms;
            last_ev_dur_ms = (ev.type == 0x1000) ? std::clamp(ev.duration_ms, 30, 500) : 32;

            int32_t xm_id = select_xiaomi_rtp_for_he(ev);
            float scale = g_live_he_amp_scale.load(std::memory_order_relaxed);
            float ev_amp = scale * (std::clamp(ev.intensity, 10, 100) / 100.0f);
            if (ev_amp < 0.08f) ev_amp = 0.08f;
            if (ev_amp > 1.0f) ev_amp = 1.0f;
            int8_t strength = (ev_amp >= 0.72f) ? 2 : (ev_amp >= 0.38f ? 1 : 0);

            LOGI("HE event[%zu/%zu] type=0x%x t=%dms dur=%dms f=%d int=%d -> xm_rtp=%d amp=%.3f",
                 i + 1, job.he_events.size(), ev.type, ev.rel_ms, ev.duration_ms, ev.freq, ev.intensity, xm_id, ev_amp);
            xm_set_amplitude_and_perform(xm_id, strength, ev_amp);
        }

        if (l + 1 < loops) {
            // Wait for the final event's tail + caller interval so loop N+1 never overlaps loop N!
            int32_t tail_wait = std::max(80, last_ev_dur_ms) + std::max(0, job.he_interval_ms);
            if (!sleep_cancellable_ms(job.gen, tail_wait)) {
                xm_off();
                return;
            }
        }
    }
    if (g_play_gen.load(std::memory_order_acquire) == job.gen) {
        fire_richtap_callback(job.he_cb);
    }
}

static void vib_on_direct(AIBinder* real_vib, int32_t ms) {
    AParcel *in = nullptr, *out = nullptr;
    if (g_ndk.prepareTransaction(real_vib, &in) == 0) {
        g_ndk.AParcel_writeInt32(in, ms);
        g_ndk.AParcel_writeStrongBinder(in, nullptr);
        g_ndk.transact(real_vib, 3, &in, &out, 0);
        if (out) g_ndk.AParcel_delete(out);
    }
}

static void vib_off_direct(AIBinder* real_vib) {
    AParcel *in = nullptr, *out = nullptr;
    if (g_ndk.prepareTransaction(real_vib, &in) == 0) {
        g_ndk.transact(real_vib, 2, &in, &out, 0);
        if (out) g_ndk.AParcel_delete(out);
    }
}

static void run_ring_job(const PlayJob& job) {
    const RingPattern* pat = job.ring_pat;
    if (!pat || !job.ring_real_vib) return;
    int max_loops = pat->loop ? 60 : 1;
    for (int l = 0; l < max_loops; ++l) {
        for (const auto& seg : pat->segs) {
            if (seg.amp > 0) {
                float a = std::clamp(seg.amp / 255.0f, 0.10f, 1.0f);
                // Short rhythmic taps use instant-onset crisp RTP waveforms for rich musical ringtone texture!
                if (seg.ms <= 45) {
                    int32_t rid = (seg.amp >= 220) ? 4 : (seg.amp >= 140 ? 166 : 181);
                    xm_set_amplitude_and_perform(rid, (seg.amp >= 200 ? 2 : 1), a);
                } else {
                    vib_on_direct(job.ring_real_vib, seg.ms);
                }
            } else {
                vib_off_direct(job.ring_real_vib);
            }
            if (!sleep_cancellable_ms(job.gen, seg.ms)) {
                vib_off_direct(job.ring_real_vib);
                return;
            }
        }
    }
    vib_off_direct(job.ring_real_vib);
    fire_vib_callback(job.ring_cb);
}

static void* worker_thread_main(void*) {
    for (;;) {
        PlayJob job;
        pthread_mutex_lock(&g_work_mu);
        while (!g_has_job) {
            pthread_cond_wait(&g_work_cv, &g_work_mu);
        }
        job = std::move(g_pending_job);
        g_has_job = false;
        pthread_mutex_unlock(&g_work_mu);

        if (job.kind == PlayJob::HE) {
            run_he_job(job);
            if (job.he_cb) g_ndk.decStrong(job.he_cb);
        } else if (job.kind == PlayJob::RINGTONE) {
            run_ring_job(job);
            if (job.ring_real_vib) g_ndk.decStrong(job.ring_real_vib);
            if (job.ring_cb) g_ndk.decStrong(job.ring_cb);
        }
    }
    return nullptr;
}

static void start_worker_thread() {
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_work_cv, &cattr);
    pthread_condattr_destroy(&cattr);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, worker_thread_main, nullptr);
    pthread_attr_destroy(&attr);
}

// ============================================================================
// AAC RichTap HE 1.0 & HE 2.0 int32[] Parser
// Matches ColorOS RichtapPatternHe (and ColorOSVibBridge::richtap_he_duration)
// ============================================================================
static int32_t parse_he_pattern(const std::vector<int32_t>& arr, std::vector<HeEvent>& out) {
    out.clear();
    if (arr.size() < 4) return 0;
    int32_t ver = arr[0];
    int32_t max_end = 0;

    if (ver == 1) {
        // HE 1.0: [1, (17 ints per event)...]
        // Event layout (17 ints): [type, relTime, intensity, freq, duration, ...]
        size_t pos = 1;
        while (pos + 17 <= arr.size()) {
            int32_t type = arr[pos];
            if (type != 0x1000 && type != 0x1001) break;
            HeEvent ev{};
            ev.type = type;
            ev.rel_ms = std::max(0, arr[pos + 1]);
            ev.intensity = std::clamp(arr[pos + 2], 1, 100);
            ev.freq = std::clamp(arr[pos + 3], 0, 100);
            ev.duration_ms = (type == 0x1000) ? std::max(15, arr[pos + 4]) : 48;
            out.push_back(ev);
            max_end = std::max(max_end, ev.rel_ms + ev.duration_ms);
            pos += 17;
        }
    } else if (ver == 2 && arr.size() >= 16) {
        // HE 2.0: 8-int header: [2, pid, ?, ?, ?, ?, absTime, numEvents]
        // Followed by pattern blocks: each pattern starts with 3-int subheader when multi-pattern!
        // Event layout at idx (starting at 8):
        //   arr[idx + 0] = type (0x1000 continuous or 0x1001 transient)
        //   arr[idx + 1] = len (payload ints after [type, len], so next event is idx + 2 + len)
        //   arr[idx + 2] = index/id
        //   arr[idx + 3] = relTime (within pattern)
        //   arr[idx + 4] = intensity (0..100)
        //   arr[idx + 5] = frequency (0..100)
        //   arr[idx + 6] = duration (for 0x1000 continuous; for 0x1001 transient -> 48ms)
        int32_t abs_time = std::max(0, arr[6]);
        int32_t num_events = arr[7];
        size_t idx = 8;
        int32_t ev_in_pat = 0;

        while (idx + 6 < arr.size() && out.size() < 256) {
            int32_t type = arr[idx];
            if (type != 0x1000 && type != 0x1001) break;
            int32_t block_len = arr[idx + 1];
            if (block_len < 4 || idx + 2 + (size_t)block_len > arr.size()) break;

            HeEvent ev{};
            ev.type = type;
            int32_t rel = std::max(0, arr[idx + 3]);
            ev.rel_ms = abs_time + rel;
            ev.intensity = std::clamp(arr[idx + 4], 1, 100);
            ev.freq = std::clamp(arr[idx + 5], 0, 100);
            ev.duration_ms = (type == 0x1000 && idx + 6 < arr.size()) ? std::max(15, arr[idx + 6]) : 48;
            out.push_back(ev);
            max_end = std::max(max_end, ev.rel_ms + ev.duration_ms);

            size_t next_idx = idx + 2 + (size_t)block_len;
            ev_in_pat++;
            if (ev_in_pat >= num_events) {
                // Check if another pattern block follows (3-int pattern header: [pid, absTime, numEvents])
                if (next_idx + 3 < arr.size()) {
                    abs_time = std::max(0, arr[next_idx + 1]);
                    num_events = arr[next_idx + 2];
                    ev_in_pat = 0;
                    idx = next_idx + 3;
                    continue;
                } else {
                    break;
                }
            }
            idx = next_idx;
        }
    }

    if (!out.empty()) {
        std::sort(out.begin(), out.end(), [](const HeEvent& a, const HeEvent& b) {
            return a.rel_ms < b.rel_ms;
        });
    }
    return std::max(max_end, 48);
}

// ============================================================================
// Raw Binder Forwarding Helper
// ============================================================================
static binder_status_t forward_raw(AIBinder* target, uint32_t code, const AParcel* in, int32_t in_pos, AParcel* out) {
    AParcel* fwd_in = nullptr;
    AParcel* fwd_out = nullptr;
    binder_status_t st = g_ndk.prepareTransaction(target, &fwd_in);
    if (st != 0) return st;
    int32_t sz = g_ndk.AParcel_getDataSize(in);
    if (sz > in_pos) {
        g_ndk.AParcel_appendFrom(in, fwd_in, in_pos, sz - in_pos);
    }
    st = g_ndk.transact(target, code, &fwd_in, &fwd_out, 0);
    if (st == 0 && fwd_out && out) {
        int32_t osz = g_ndk.AParcel_getDataSize(fwd_out);
        if (osz > 0) g_ndk.AParcel_appendFrom(fwd_out, out, 0, osz);
    }
    if (fwd_out) g_ndk.AParcel_delete(fwd_out);
    return st;
}

static bool i32_vec_allocator(void* userdata, int32_t len, int32_t** out_buf) {
    auto* v = static_cast<std::vector<int32_t>*>(userdata);
    if (len < 0 || len > 65536) return false;
    v->resize((size_t)len);
    *out_buf = v->empty() ? nullptr : v->data();
    return true;
}

// ============================================================================
// IOplusVibrator Extension Proxy (intercepts code 12 performHe, code 6 updateHeParam,
// code 7/14 off, and cancels active HE on preempting playback codes)
// ============================================================================
static void* passthrough_create(void* args) { return args; }
static void passthrough_destroy(void* userData) {
    if (userData) g_ndk.decStrong(static_cast<AIBinder*>(userData));
}
static binder_status_t dummy_cb_transact(AIBinder*, uint32_t, const AParcel*, AParcel*) { return 0; }

static binder_status_t ext_on_transact(AIBinder* binder, uint32_t code, const AParcel* in, AParcel* out) {
    AIBinder* real_ext = static_cast<AIBinder*>(g_ndk.getUserData(binder));
    if (!real_ext) return -32;
    int32_t in_pos = g_ndk.AParcel_getDataPosition(in);

    // Code 6: updateHeParameter(int32 interval, int32 amplitude, int32 freq, IBinder cb)
    // ColorOS OplusBootReg / RichTap stops active HE playback by calling code 6 with (0, 0, 0)!
    if (code == 6) {
        int32_t interval = 0, amplitude = 0, freq = 0;
        g_ndk.AParcel_readInt32(in, &interval);
        g_ndk.AParcel_readInt32(in, &amplitude);
        g_ndk.AParcel_readInt32(in, &freq);
        if ((interval | amplitude | freq) == 0 || amplitude <= 0) {
            LOGI("updateHeParameter code=6 STOP (int=%d amp=%d freq=%d) -> cancel HE", interval, amplitude, freq);
            cancel_active_playback();
            xm_off();
        } else {
            float new_scale = std::clamp(amplitude / 255.0f, 0.12f, 1.0f);
            g_live_he_amp_scale.store(new_scale, std::memory_order_relaxed);
            LOGI("updateHeParameter code=6 UPDATE (int=%d amp=%d freq=%d -> scale=%.3f)", interval, amplitude, freq, new_scale);
        }
        return forward_raw(real_ext, code, in, in_pos, out);
    }

    // Code 7 or Code 14: stop/off -> cancel active HE playback + forward
    if (code == 7 || code == 14) {
        cancel_active_playback();
        xm_off();
        return forward_raw(real_ext, code, in, in_pos, out);
    }

    // Code 12: performHe(int32 looper, int32 interval, int32 amplitude, int32 freq, int32[] pattern, IBinder cb)
    if (code == 12) {
        int32_t looper = 1, interval = 0, amplitude = 255, freq = 0;
        std::vector<int32_t> pattern;
        AIBinder* cb = nullptr;
        g_ndk.AParcel_readInt32(in, &looper);
        g_ndk.AParcel_readInt32(in, &interval);
        g_ndk.AParcel_readInt32(in, &amplitude);
        g_ndk.AParcel_readInt32(in, &freq);
        g_ndk.AParcel_readInt32Array(in, &pattern, i32_vec_allocator);
        g_ndk.AParcel_readStrongBinder(in, &cb);

        std::vector<HeEvent> events;
        int32_t dur_ms = parse_he_pattern(pattern, events);
        LOGI("performHe code=12 len=%zu ver=%d events=%zu dur=%dms amp=%d loop=%d interval=%d",
             pattern.size(), pattern.empty() ? -1 : pattern[0], events.size(), dur_ms, amplitude, looper, interval);

        if (!events.empty()) {
            cancel_active_playback();
            float amp_scale = std::clamp((amplitude <= 0 ? 255 : amplitude) / 255.0f, 0.12f, 1.0f);
            g_live_he_amp_scale.store(amp_scale, std::memory_order_relaxed);
            pthread_mutex_lock(&g_work_mu);
            uint64_t next_gen = g_play_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
            g_pending_job.kind = PlayJob::HE;
            g_pending_job.gen = next_gen;
            g_pending_job.he_events = std::move(events);
            g_pending_job.he_looper = looper;
            g_pending_job.he_interval_ms = interval;
            g_pending_job.he_amp_scale = amp_scale;
            g_pending_job.he_cb = cb;  // ownership transferred
            g_has_job = true;
            pthread_cond_broadcast(&g_work_cv);
            pthread_mutex_unlock(&g_work_mu);

            g_ndk.AParcel_writeInt32(out, 0);       // EX_NONE
            g_ndk.AParcel_writeInt32(out, dur_ms);  // durationMs
            return 0;
        }
        if (cb) g_ndk.decStrong(cb);
        // Empty/unrecognized pattern -> fall through to ColorOSVibBridge
        return forward_raw(real_ext, code, in, in_pos, out);
    }

    // Preempting IOplusVibrator playback codes: cancel any active HE job before forwarding
    if (code == 2 || code == 3 || code == 4 || code == 5 ||
        code == 8 || code == 9 || code == 10 || code == 11 || code == 13) {
        cancel_active_playback();
    }

    return forward_raw(real_ext, code, in, in_pos, out);
}

// ============================================================================
// IVibrator Wrapper Cache (wraps IVibrator + attaches IOplusVibrator extension!)
// ============================================================================
struct VibSlot { AIBinder* real = nullptr; AIBinder* wrap = nullptr; };
static pthread_mutex_t g_wrap_mu = PTHREAD_MUTEX_INITIALIZER;
static std::vector<VibSlot> g_vib_slots;

static AIBinder* wrap_vibrator(AIBinder* real_vib) {
    if (!real_vib) return nullptr;
    pthread_mutex_lock(&g_wrap_mu);
    for (auto& s : g_vib_slots) {
        if (!g_ndk.lt(s.real, real_vib) && !g_ndk.lt(real_vib, s.real)) {
            g_ndk.incStrong(s.wrap);
            pthread_mutex_unlock(&g_wrap_mu);
            return s.wrap;
        }
    }
    g_ndk.associateClass(real_vib, g_cls_vib);
    g_ndk.incStrong(real_vib);
    AIBinder* wrap = g_ndk.new_binder(g_cls_vib, real_vib);
    if (g_ndk.markVintfStability) g_ndk.markVintfStability(wrap);

    // CRITICAL FIX: Forward IOplusVibrator extension from ColorOSVibBridge!
    if (g_ndk.getExtension && g_ndk.setExtension) {
        AIBinder* real_ext = nullptr;
        binder_status_t est = g_ndk.getExtension(real_vib, &real_ext);
        if (est == 0 && real_ext) {
            g_ndk.associateClass(real_ext, g_cls_ext);
            // real_ext already has a strong ref from getExtension; transfer ownership to ext_wrap
            AIBinder* ext_wrap = g_ndk.new_binder(g_cls_ext, real_ext);
            if (g_ndk.markVintfStability) g_ndk.markVintfStability(ext_wrap);
            binder_status_t sst = g_ndk.setExtension(wrap, ext_wrap);
            LOGI("attached IOplusVibrator extension real_ext=%p ext_wrap=%p st=%d", real_ext, ext_wrap, sst);
            g_ndk.decStrong(ext_wrap);
        } else {
            LOGW("real_vib %p has no extension (st=%d)", real_vib, est);
        }
    }

    g_vib_slots.push_back({real_vib, wrap});
    g_ndk.incStrong(wrap);
    LOGI("wrapped IVibrator slot %zu real=%p wrap=%p", g_vib_slots.size() - 1, real_vib, wrap);
    pthread_mutex_unlock(&g_wrap_mu);
    return wrap;
}

static binder_status_t vib_on_transact(AIBinder* binder, uint32_t code, const AParcel* in, AParcel* out) {
    AIBinder* real_vib = static_cast<AIBinder*>(g_ndk.getUserData(binder));
    if (!real_vib) return -32;
    int32_t in_pos = g_ndk.AParcel_getDataPosition(in);

    // Code 2: off() -> cancel any active HE or ringtone pattern, then forward
    if (code == 2) {
        cancel_active_playback();
        xm_off();
        return forward_raw(real_vib, code, in, in_pos, out);
    }

    // Code 3: on(timeoutMs, cb) -> cancel active pattern, then forward
    if (code == 3) {
        cancel_active_playback();
        return forward_raw(real_vib, code, in, in_pos, out);
    }

    // Code 4: perform(effect, strength, cb) -> check /vendor/etc/xmvibproxy.conf ringtone patterns
    if (code == 4) {
        int32_t effect = -1, strength = -1;
        AIBinder* cb = nullptr;
        g_ndk.AParcel_readInt32(in, &effect);
        g_ndk.AParcel_readInt32(in, &strength);
        g_ndk.AParcel_readStrongBinder(in, &cb);

        const RingPattern* pat = find_ring_pattern(effect);
        if (pat) {
            cancel_active_playback();
            pthread_mutex_lock(&g_work_mu);
            uint64_t next_gen = g_play_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
            g_pending_job.kind = PlayJob::RINGTONE;
            g_pending_job.gen = next_gen;
            g_pending_job.ring_pat = pat;
            g_ndk.incStrong(real_vib);
            g_pending_job.ring_real_vib = real_vib;
            g_pending_job.ring_cb = cb;  // ownership transferred
            g_has_job = true;
            pthread_cond_broadcast(&g_work_cv);
            pthread_mutex_unlock(&g_work_mu);

            int32_t dur = pat->loop ? 60000 : pat->total_ms;
            LOGI("perform ringtone id=%d strength=%d -> %s %zu segs %dms",
                 effect, strength, pat->loop ? "loop" : "once", pat->segs.size(), dur);
            g_ndk.AParcel_writeInt32(out, 0);
            g_ndk.AParcel_writeInt32(out, dur);
            return 0;
        }
        if (cb) g_ndk.decStrong(cb);
        cancel_active_playback();
        return forward_raw(real_vib, code, in, in_pos, out);
    }

    return forward_raw(real_vib, code, in, in_pos, out);
}

// ============================================================================
// IVibratorManager Proxy (wraps getVibrator code 3)
// ============================================================================
static binder_status_t mgr_on_transact(AIBinder*, uint32_t code, const AParcel* in, AParcel* out) {
    if (!g_real_mgr) return -32;
    int32_t in_pos = g_ndk.AParcel_getDataPosition(in);
    if (code != 3) {
        return forward_raw(g_real_mgr, code, in, in_pos, out);
    }
    AParcel *fwd_in = nullptr, *fwd_out = nullptr;
    binder_status_t st = g_ndk.prepareTransaction(g_real_mgr, &fwd_in);
    if (st != 0) return st;
    int32_t sz = g_ndk.AParcel_getDataSize(in);
    if (sz > in_pos) g_ndk.AParcel_appendFrom(in, fwd_in, in_pos, sz - in_pos);
    st = g_ndk.transact(g_real_mgr, 3, &fwd_in, &fwd_out, 0);
    if (st != 0 || !fwd_out) {
        if (fwd_out) g_ndk.AParcel_delete(fwd_out);
        return st;
    }
    int32_t exc = -1;
    g_ndk.AParcel_readInt32(fwd_out, &exc);
    if (exc != 0) {
        int32_t osz = g_ndk.AParcel_getDataSize(fwd_out);
        g_ndk.AParcel_appendFrom(fwd_out, out, 0, osz);
        g_ndk.AParcel_delete(fwd_out);
        return 0;
    }
    AIBinder* real_v = nullptr;
    g_ndk.AParcel_readStrongBinder(fwd_out, &real_v);
    g_ndk.AParcel_delete(fwd_out);

    AIBinder* wrapped = real_v ? wrap_vibrator(real_v) : nullptr;
    g_ndk.AParcel_writeInt32(out, 0);
    g_ndk.AParcel_writeStrongBinder(out, wrapped ? wrapped : real_v);
    if (wrapped) g_ndk.decStrong(wrapped);
    if (real_v) g_ndk.decStrong(real_v);
    return 0;
}

static void on_real_died(void* cookie) {
    LOGE("real vibrator hal died (%s), exit", cookie ? (const char*)cookie : "unknown");
    _exit(1);
}

static bool resolve_ndk() {
    void* lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!lib) { LOGE("dlopen libbinder_ndk.so failed: %s", dlerror()); return false; }
#define SYM(field, name) do { g_ndk.field = (decltype(g_ndk.field))dlsym(lib, name); \
    if (!g_ndk.field) { LOGE("missing symbol %s", name); return false; } } while (0)
    SYM(Class_define, "AIBinder_Class_define");
    SYM(new_binder, "AIBinder_new");
    SYM(getUserData, "AIBinder_getUserData");
    SYM(incStrong, "AIBinder_incStrong");
    SYM(decStrong, "AIBinder_decStrong");
    SYM(isRemote, "AIBinder_isRemote");
    SYM(associateClass, "AIBinder_associateClass");
    SYM(lt, "AIBinder_lt");
    SYM(prepareTransaction, "AIBinder_prepareTransaction");
    SYM(transact, "AIBinder_transact");
    SYM(DeathRecipient_new, "AIBinder_DeathRecipient_new");
    SYM(linkToDeath, "AIBinder_linkToDeath");
    SYM(AParcel_delete, "AParcel_delete");
    SYM(AParcel_getDataPosition, "AParcel_getDataPosition");
    SYM(AParcel_setDataPosition, "AParcel_setDataPosition");
    SYM(AParcel_getDataSize, "AParcel_getDataSize");
    SYM(AParcel_appendFrom, "AParcel_appendFrom");
    SYM(AParcel_readInt32, "AParcel_readInt32");
    SYM(AParcel_writeInt32, "AParcel_writeInt32");
    SYM(AParcel_writeFloat, "AParcel_writeFloat");
    SYM(AParcel_readStrongBinder, "AParcel_readStrongBinder");
    SYM(AParcel_writeStrongBinder, "AParcel_writeStrongBinder");
    SYM(AParcel_readInt32Array, "AParcel_readInt32Array");
    SYM(checkService, "AServiceManager_checkService");
    SYM(waitForService, "AServiceManager_waitForService");
    SYM(addService, "AServiceManager_addService");
    SYM(setThreadPoolMaxThreadCount, "ABinderProcess_setThreadPoolMaxThreadCount");
    SYM(startThreadPool, "ABinderProcess_startThreadPool");
    SYM(joinThreadPool, "ABinderProcess_joinThreadPool");
#undef SYM
    g_ndk.markVintfStability = (decltype(g_ndk.markVintfStability))dlsym(lib, "AIBinder_markVintfStability");
    g_ndk.getExtension = (decltype(g_ndk.getExtension))dlsym(lib, "AIBinder_getExtension");
    g_ndk.setExtension = (decltype(g_ndk.setExtension))dlsym(lib, "AIBinder_setExtension");
    return true;
}

int main() {
    if (!resolve_ndk()) return 1;
    load_ring_conf();
    start_worker_thread();

    g_cls_vib = g_ndk.Class_define("android.hardware.vibrator.IVibrator", passthrough_create, passthrough_destroy, vib_on_transact);
    g_cls_mgr = g_ndk.Class_define("android.hardware.vibrator.IVibratorManager", passthrough_create, passthrough_destroy, mgr_on_transact);
    g_cls_ext = g_ndk.Class_define("vendor.oplus.hardware.oplusvibrator.IOplusVibrator", passthrough_create, passthrough_destroy, ext_on_transact);
    g_cls_cb = g_ndk.Class_define("android.hardware.vibrator.IVibratorCallback", passthrough_create, passthrough_destroy, dummy_cb_transact);
    g_cls_rich_cb = g_ndk.Class_define("vendor.aac.hardware.richtap.vibrator.IRichtapCallback", passthrough_create, passthrough_destroy, dummy_cb_transact);

    // Wait cleanly at boot via checkService (avoids waitForService callback binder AVC with servicemanager)
    AIBinder* real_vib = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt) {
        real_vib = g_ndk.checkService("android.hardware.vibrator.IVibrator/default");
        if (real_vib && g_ndk.isRemote(real_vib)) break;
        if (real_vib) { g_ndk.decStrong(real_vib); real_vib = nullptr; }
        usleep(150000);
    }
    if (!real_vib || !g_ndk.isRemote(real_vib)) {
        LOGE("no remote IVibrator/default");
        return 1;
    }
    g_ndk.associateClass(real_vib, g_cls_vib);

    // Self-check getCapabilities (code 1)
    {
        AParcel *in = nullptr, *out = nullptr;
        binder_status_t st = g_ndk.prepareTransaction(real_vib, &in);
        int32_t exc = -1, cap = 0;
        if (st == 0) st = g_ndk.transact(real_vib, 1, &in, &out, 0);
        if (st == 0 && out) {
            g_ndk.AParcel_readInt32(out, &exc);
            g_ndk.AParcel_readInt32(out, &cap);
        }
        if (out) g_ndk.AParcel_delete(out);
        LOGI("self-check getCapabilities st=%d ex=%d cap=0x%x", st, exc, cap);
        if (st != 0 || exc != 0) {
            LOGE("self-check failed, keep hal untouched");
            return 1;
        }
    }

    auto* death = g_ndk.DeathRecipient_new(on_real_died);
    g_ndk.linkToDeath(real_vib, death, (void*)"IVibrator/default");

    AIBinder* real_mgr = g_ndk.checkService("android.hardware.vibrator.IVibratorManager/default");
    if (real_mgr && g_ndk.isRemote(real_mgr)) {
        g_ndk.associateClass(real_mgr, g_cls_mgr);
        g_real_mgr = real_mgr;
        g_ndk.linkToDeath(real_mgr, death, (void*)"IVibratorManager/default");
    }

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();

    AIBinder* wrap_vib = wrap_vibrator(real_vib);
    binder_status_t st1 = g_ndk.addService(wrap_vib, "android.hardware.vibrator.IVibrator/default");
    LOGI("addService android.hardware.vibrator.IVibrator/default -> %d", st1);

    if (g_real_mgr) {
        AIBinder* wrap_mgr = g_ndk.new_binder(g_cls_mgr, nullptr);
        if (g_ndk.markVintfStability) g_ndk.markVintfStability(wrap_mgr);
        binder_status_t st2 = g_ndk.addService(wrap_mgr, "android.hardware.vibrator.IVibratorManager/default");
        LOGI("addService android.hardware.vibrator.IVibratorManager/default -> %d", st2);
    }

    g_ndk.joinThreadPool();
    return 0;
}
