# Display, Zero-Polling LTPO, HDR Peak & CWB Deployment Notes

**Target Base Port:** ColorOS 17 V6 CN (`PLZ110_17.0.0.107(CN01)`, OnePlus 15T base) ported to Xiaomi 15 (`dada`, SM8750)

## 1. Under-Display CWB Throttling (`vendor_cwb_display_props.patch`)
- **Problem:** Xiaomi 15's BOLED TCS3720 under-display ALS/proximity stack (`libclient2slpi.notifier.so`) captures a SurfaceFlinger Client WriteBack (CWB) frame every `200 ms` (`5 Hz`) during continuous frame updates (scrolling / 60fps video).
- **Fix:** Setting `ro.vendor.cwb.timeout=1000` throttles steady-state CWB captures to `1 Hz` (`-79.5%` CWB triggers during 4K60 video playback) while preserving instant `150 ms` `misForced=1` CWB on wake/settle and `100%` ALS/proximity accuracy.
- **Dirty-Flash Safety:** `src/init/init.cos17_dada_perf.rc` ensures `persist.vendor.debug.cwb.disable` is reset to `false` if previously set by older experiments.

## 2. Zero-Polling Hardware 1–120 Hz LTPO (`overlay.cpp` + `ltpofix_plugin.cpp` + `LtpoFixHooks.java`)
- **Problem:** Legacy LTPO hooks spawned a `10 Hz` `publishUni` thread inside SurfaceFlinger (`1.99 MB` `overlay.so`) and polled a `1 MB` `memfd` snapshot buffer from `system_server`, keeping `SurfaceFlinger`, `crtc_commit`, and `system_server` continuously awake.
- **Fix:**
  - `src/ltpo/overlay.cpp` (`12 KB` replacement `/system/lib64/overlay.so`, `DT_NEEDED` by `surfaceflinger`) installs a minimal pre-hook on `SurfaceFlinger::composite()` recording frame timestamps in a lock-free ring buffer (`~150 ns/frame`, `0` heap allocs) and drives a `CLOCK_MONOTONIC` condvar watchdog (`0 -> 10 -> 1 Hz`, then infinite `pthread_cond_wait()` = **`0.000%` CPU** at rest).
  - `src/ltpo/ltpofix_plugin.cpp` waits on kernel `__system_property_wait()` futex for `debug.nyako.ltpo.sf_idle` and wakes `LtpoFixHooks.java` via `pipe2()` (zero polling, zero `memfd` copies, `0.06%` SF CPU at 1 Hz).
  - Deploy `src/init/ltpo_overlay.rc` to `/system/etc/init/ltpo_overlay.rc`.

## 3. Event-Driven 3200-Nit HDR Peak Daemon (`hdrpeakd.cpp`)
- **Problem:** Shell-based HDR polling burned `10.8%` CPU; an earlier C++ port initialized `uint32_t global_ser = 0` inside the retry branch of `prop_watch_thread()`, causing a `98%` CPU spin on fresh flashes until the watched property was first written.
- **Fix:** `src/daemons/hdrpeakd.cpp` registers `IHdrLayerInfoListener` on `SurfaceFlingerAIDL`, drives `IDisplayFeature` directly over AIDL as `uid=1000`, and seeds `global_ser = __system_property_area_serial()` once before entering the futex loop (**`0.000%` idle CPU**).
- Deploy `src/init/hdrpeak.rc` to `/vendor/etc/init/hdrpeak.rc`.

## 4. Zero-Loop Color Mode Sync (`scripts/patch_color_mode_sync.py` + `scripts/patch_surfaceflinger_renderintent.py`)
- **Problem:** `/odm/bin/hw/oplus-color-mode-sync` forked `/system/bin/settings` twice per second (`2 Hz`) forever (`8,854` jiffies over 70 minutes) to translate ColorOS display modes (`301/303/307`).
- **Fix:** Move RenderIntent translation directly into `/system/bin/surfaceflinger` (`301 -> 257`, `303 -> 258`, `307 -> 256`) via `scripts/patch_surfaceflinger_renderintent.py` and replace the `oplus-color-mode-sync` polling/fork loop with infinite kernel futex wait (`__system_property_wait` on `persist.sys.sf.native_mode`) via `scripts/patch_color_mode_sync.py` (`8,854j -> 0j`, `0.000%` idle CPU).
