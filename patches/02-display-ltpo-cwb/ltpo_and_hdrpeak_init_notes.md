# Display, Zero-Polling LTPO, Soft-Reboot Guard, HDR Peak & CWB Deployment Notes (`v7.3.1`)

**Target Base Port:** ColorOS 17 V6 CN (`PLZ110_17.0.0.107(CN01)`, OnePlus 15T base) ported to Xiaomi 15 (`dada`, SM8750)

## 1. Under-Display CWB Throttling (`vendor_cwb_display_props.patch`)
- **Problem:** Xiaomi 15's BOLED TCS3720 under-display ALS/proximity stack (`libclient2slpi.notifier.so`) captures a SurfaceFlinger Client WriteBack (CWB) frame every `200 ms` (`5 Hz`) during continuous frame updates (scrolling / 60fps video).
- **Fix:** Setting `ro.vendor.cwb.timeout=1000` throttles steady-state CWB captures to `1 Hz` (`-79.5%` CWB triggers during 4K60 video playback) while preserving instant `150 ms` `misForced=1` CWB on wake/settle and `100%` ALS/proximity accuracy.
- **Dirty-Flash Safety:** NEVER set `persist.vendor.debug.cwb.disable=true` (breaks BOLED ALS/Auto-Brightness). `src/init/init.cos17_dada_perf.rc` automatically resets `persist.vendor.debug.cwb.disable` to `false` on boot if set by older builds.

## 2. Zero-Polling Hardware 1–120 Hz LTPO + `RefreshRateSelector` Soft-Reboot Guard (`overlay.cpp` v4.1 + `ltpofix_plugin.cpp` v8 + `LtpoFixHooks.java` v8.1)
- **Problems Solved:**
  1. Legacy LTPO hooks spawned a `10 Hz` `publishUni` thread inside SurfaceFlinger (`1.99 MB` `overlay.so`) and polled a `1 MB` `memfd` snapshot buffer from `system_server`.
  2. In `v7.2`, unsynchronized `STATE_DOZE` / 60 Hz mode transitions sent isolated `sfModeId=3` (`120.00001 Hz` `GROUP_IDLE_1`) with narrowed `render=[0, 60]` / truncated `physical=[0, 120.00]` ranges and `allowGroupSwitching=false`, triggering a deterministic `RefreshRateSelector::constructAvailableRefreshRates` `SIGABRT` soft reboot (`tombstone_06..11`) and occasionally trapping Quick Settings in `id=3` post-Doze wake.
- **Fix (Two-Layer Zero-Polling Architecture):**
  - **`src/ltpo/overlay.cpp` (v4.1, `15 KB` `/system/lib64/overlay.so`, `DT_NEEDED` by `surfaceflinger`):**
    - Pre-hooks `SurfaceFlinger::composite()` with an ABI-preserving naked trampoline recording frame timestamps in a lock-free 64-entry ring buffer (`~150 ns/frame`, `0` heap allocs) and drives a `CLOCK_MONOTONIC` condvar watchdog (`0 -> 10 -> 1 Hz`, `kIdle10Ms = 300 ms` with instant wake from `10 Hz`, then infinite `pthread_cond_wait()` = **`0.000%` CPU** at rest).
    - Installs Trampoline 3 on **`RefreshRateSelector::setPolicy` (`+0x2cdfac`)** to clamp any narrow `DisplayManagerPolicy` for isolated 120 Hz LTPO modes (`sfModeId 1..3`) or locked 120 Hz modes (`0..5`) to `[0.0f, 120.1f]` (`8,326,394 ns`), preventing `SurfaceFlinger` `SIGABRT` crashes even when third-party refresh-rate modules are active.
    - When linking `overlay.so` for PLZ110 `surfaceflinger`, pass `STATSLOG_BOOTSTRAP_SO=/path/to/libstatslog_surfaceflinger_bootstrap.so` to `src/daemons/build_ndk.sh` so `DT_NEEDED` satisfies `surfaceflinger`'s transitive symbol dependency (`_ZN7android14surfaceflinger5stats11stats_writeEiilibf`), and keep 16 KB `PT_LOAD` alignment (`-Wl,-z,max-page-size=16384`).
  - **`src/ltpo/ltpofix_plugin.cpp` (v8) + `src/ltpo/java/com/nyako/ltpofix/LtpoFixHooks.java` (v8.1):**
    - Blocks on kernel `__system_property_wait()` futex for `debug.nyako.ltpo.sf_idle` and wakes Java via `pipe2()` (zero polling, `0.06%` SF CPU at 1 Hz).
    - Installs 4 hooks (`getDesiredDisplayModeSpecs`, `findSfDisplayModeIdLocked`, `setDefaultDisplayState`, and `setDesiredDisplayModeSpecsLocked`), synchronizes `120.1f` ranges + `vrrPolicy` before every isolated mode switch, tracks `sDirectorDisplayState`, and schedules `+120 ms` / `+320 ms` post-Doze wake resyncs (`5/5` `id=1@120.00001Hz` on Quick Settings pull, `0` restarts across `10/10` Fullscreen AOD + `5/5` torture + `3/3` reboots).
  - Deploy `src/init/ltpo_overlay.rc` to `/system/etc/init/ltpo_overlay.rc`.

## 3. Event-Driven 3200-Nit HDR Peak Daemon (`hdrpeakd.cpp`)
- **Problem:** Shell-based HDR polling burned `10.8%` CPU; an earlier C++ port initialized `uint32_t global_ser = 0` inside the retry branch of `prop_watch_thread()`, causing a `98%` CPU spin on fresh flashes until the watched property was first written.
- **Fix:** `src/daemons/hdrpeakd.cpp` registers `IHdrLayerInfoListener` on `SurfaceFlingerAIDL`, drives `IDisplayFeature` directly over AIDL as `uid=1000`, and seeds `global_ser = __system_property_area_serial()` once before entering the futex loop (**`0.000%` idle CPU**).
- Deploy `src/init/hdrpeak.rc` to `/vendor/etc/init/hdrpeak.rc`.

## 4. Zero-Loop Color Mode Sync (`scripts/patch_color_mode_sync.py` + `scripts/patch_surfaceflinger_renderintent.py`)
- **Problem:** `/odm/bin/hw/oplus-color-mode-sync` forked `/system/bin/settings` twice per second (`2 Hz`) forever (`8,854` jiffies over 70 minutes) to translate ColorOS display modes (`301/303/307`).
- **Fix:** Move RenderIntent translation directly into `/system/bin/surfaceflinger` (`301 -> 257`, `303 -> 258`, `307 -> 256`) via `scripts/patch_surfaceflinger_renderintent.py` and replace the `oplus-color-mode-sync` polling/fork loop with infinite kernel futex wait (`__system_property_wait` on `persist.sys.sf.native_mode`) via `scripts/patch_color_mode_sync.py` (`8,854j -> 0j`, `0.000%` idle CPU).
