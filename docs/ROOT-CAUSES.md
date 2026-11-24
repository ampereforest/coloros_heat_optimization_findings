# Forensic Root-Cause Catalog (`ROOT-CAUSES.md`)

**Target Base Port:** ColorOS 17 V6 CN (`PLZ110_17.0.0.107(CN01)`, OnePlus 15T base) ported to Xiaomi 15 (`dada`, SM8750 / HyperOS `OS4.0.0.11` vendor base).

This document catalogs every root cause identified through live thermal, CPU/GPU residency, softirq, SELinux audit, Binder, and power-rail profiling, together with the exact non-destructive fix and critical anti-patterns to avoid.

---

## 1. Thermal HAL & Perf Moderation (Zero Clock-Capping)

### 1.1 Unthrottled `mi_thermald` Scenario Runaway (`sconfig=50` / `sconfig=6` -> `sconfig=0`)
- **Root Cause:** ColorOS 17 left Xiaomi's `mi_thermald` in `sconfig=50` (`GL-PER-NORMAL` Performance Mode) or `sconfig=6` instead of Xiaomi 15's stock balanced `sconfig=0` (`GL-NORMAL`). Under `sconfig=50`, `[TEMP_AWARE]` dynamic thermal stepping is inactive, allowing board and CPU die temperatures to climb past `50 °C` during ordinary scrolling and screen-off hotspot/5G operation.
- **Fix:** Write `0` to `/sys/class/thermal/thermal_message/sconfig` in `src/init/init.cos17_dada_perf.rc` (`sys.boot_completed=1`, `vendor.post_boot.parsed=1`, and `sys.user.0.ce_available=true`). Full `3.53 GHz` Medium and `4.32 GHz` Prime frequencies remain 100% uncapped in `cpufreq`.

### 1.2 `<PerfBreak/>` Thermal-Override Tables (`/odm/etc/` & `/vendor/etc/perf/`)
- **Root Cause:** ColorOS 17 (`PLZ110`) shipped active `<PerfBreak>` tables in both `/odm/etc/thermalbreakboostconfig.xml` and `/vendor/etc/perf/thermalbreakboostconfig.xml` (absent on ColorOS 16 Beta 3 and Stock HyperOS). On every UI scroll (`Hint="4224"`) and app launch (`Hint="4225"`, `"4226"`), `urcc`/`uah` overrode active thermal ceilings and pinned Medium cores to `2.40–2.78 GHz` and Prime cores to `2.25–4.09 GHz` even when board temperature was already `39–41 °C` (`50–53 °C` CPU die).
- **Fix:** Replace both tables with empty `<BoostConfigs><PerfBreak/></BoostConfigs>` (`patches/01-thermal-perf/vendor_thermalbreakboostconfig.patch` and `odm_thermalbreakboostconfig.xml`).

### 1.3 Inflated QTI PerfHAL Boost Floors (`vendor/etc/perf/perfboostsconfig.xml`)
- **Root Cause:** Base `perfboostsconfig.xml` pinned `MIN_FREQ = 0xFFF` on launch/animation boosts (`0x00001081`, `0x00001083`), disabled CPU deep-sleep LPM (`0x40408000`) during scrolls/flings/drags, forced Prime cores online, and applied a **5,000 ms `680 MHz` GPU floor** + `1500 MHz` BIG floor on `Large Comp Hint` (`0x00001097`) during ordinary Home screen swipes.
- **Fix:** `patches/01-thermal-perf/vendor_perfboostsconfig.patch` removes `min_freq = 0xFFF`, removes LPM disable opcodes, removes the 5-second `680 MHz` GPU floor, and moderates scroll/launch floors while leaving `MAX_FREQ` (`0x40804000`, `0x40804200`) **100% uncapped**. `patches/01-thermal-perf/vendor_perfconfigstore.patch` enables `ro.vendor.perf.sagt=true` on `Target="sun"`.

### 1.4 Flattened Thermal FPS Table & Wrong Thermal Zone Path
- **Root Cause:**
  - `/vendor/etc/display/thermallevel_to_fps.xml` in the base port flattened thermal levels `1..10` to `144 fps`, disabling Xiaomi 15's stock thermal FPS step-down (`144 -> 120 -> 90 -> 60 fps`).
  - `/odm/etc/ThermalServiceConfig/sys_thermal_config.xml` and `/my_stock/etc/ThermalServiceConfig/sys_thermal_config.xml` enabled DCS/log/errlog uploads (`1`) and pointed `<thermal_heat_path>` to non-existent `/sys/class/thermal/thermal_zone84/temp` instead of Xiaomi 15's `/sys/class/thermal/thermal_zone57/temp`.
- **Fix:** `patches/01-thermal-perf/vendor_thermallevel_to_fps.patch`, `odm_sys_thermal_config.patch`, and `my_stock_sys_thermal_config.patch`.

---

## 2. Eliminating Background Polling Loops (Zero-Polling Native Architecture)

### 2.1 `oplus-color-mode-sync` 2 Hz `/system/bin/settings` Fork Storm
- **Root Cause:** `/odm/bin/hw/oplus-color-mode-sync` spawned `/system/bin/settings get system ...` every `500 ms` (`2 Hz`) forever to keep ColorOS display color modes (`301`/`303`/`307`) mapped to Xiaomi's panel modes, burning **`8,854` cumulative jiffies** over a 70-minute settled run and repeatedly waking `system_server`.
- **Fix:**
  1. `scripts/patch_surfaceflinger_renderintent.py` injects BTI-safe AArch64 helpers in `/system/bin/surfaceflinger`'s zero-padded `.text` caves (`0x646120`, `0x646150`) hooked from the three `mActiveColorMode` stores (`0x4fda14`, `0x288230`, `0x4fe948`), translating `301 -> 257`, `303 -> 258`, and `307 -> 256` directly inside SurfaceFlinger.
  2. `scripts/patch_color_mode_sync.py` replaces `/odm/bin/hw/oplus-color-mode-sync`'s `100 ms` polling + `500 ms` settings fork loop with 100% event-driven infinite kernel futex sleep (`__system_property_wait` with `timeout = NULL`) on `persist.sys.sf.native_mode` (`8,854j -> 0j`, `0.000%` idle CPU, instant `<1 ms` response on color mode changes).

### 2.2 `hdrpeakd` Polling & Fresh-Flash `98%` CPU Spin Bug
- **Root Cause:** The legacy shell HDR helper polled `dumpsys SurfaceFlinger` continuously (`10.82%` CPU). An early native C++ port introduced a subtle bug in `prop_watch_thread()`: `uint32_t global_ser = 0;` was re-initialized inside the `if (!pi)` retry branch, causing `__system_property_wait(nullptr, 0, &global_ser, nullptr)` to return immediately in a **98% CPU spin loop** on fresh flashes whenever a watched property (`persist.sys.oplus.keyguard.showing`) did not yet exist.
- **Fix:** `src/daemons/hdrpeakd.cpp` registers `IHdrLayerInfoListener` on `SurfaceFlingerAIDL`, drives Xiaomi's `IDisplayFeature` AIDL directly as `uid=1000`, and seeds `uint32_t global_ser = __system_property_area_serial();` **once** before entering the `while (!g_stop)` futex wait loop — achieving **`0.000%` idle CPU** while maintaining full 3200-nit Ultra HDR & SDR2HDR support.

### 2.3 Hardware 1–120 Hz LTPO (`futex -> pipe2()`, Eliminating `memfd` / `dumpsys` Polling)
- **Root Cause:** The original `1.99 MB` `/system/lib64/overlay.so` ran a `10 Hz` `publishUni` thread inside `SurfaceFlinger`, while `libNyako_hook_ltpofix.so` polled a `1 MB` `memfd` snapshot buffer from `system_server`, keeping `surfaceflinger`, `crtc_commit`, and `system_server` busy even on a static screen.
- **Fix:**
  - `src/ltpo/overlay.cpp` (`12 KB` replacement `overlay.so`) pre-hooks `SurfaceFlinger::composite()` with an ABI-preserving naked trampoline recording frame timestamps in a 64-entry ring buffer (`~150 ns/frame`, zero heap allocs) and drives a `CLOCK_MONOTONIC` condvar watchdog (`0 -> 10 -> 1 Hz`, then infinite `pthread_cond_wait()` = **`0.000%` CPU** at rest).
  - `src/ltpo/ltpofix_plugin.cpp` blocks on `__system_property_wait()` futex for `debug.nyako.ltpo.sf_idle` and writes a 1-byte event into `pipe2()`, waking `src/ltpo/java/com/nyako/ltpofix/LtpoFixHooks.java` directly (`0.06%` SF CPU at 1 Hz).

### 2.4 Under-Display CWB Proximity/ALS Capture Rate (`ro.vendor.cwb.timeout=1000`)
- **Root Cause:** Xiaomi 15's BOLED TCS3720 under-display ALS/proximity library (`libclient2slpi.notifier.so`) captures a SurfaceFlinger Client WriteBack (CWB) framebuffer every `200 ms` (`5 Hz`) during continuous display updates (video playback / scrolling).
- **Fix:** Setting `ro.vendor.cwb.timeout=1000` (`patches/02-display-ltpo-cwb/vendor_cwb_display_props.patch`) reduces steady-state CWB triggers by **`-79.5%`** (`39 -> 8` per 5 s during 4K60 video) with `0%` screen-off leak while preserving instant `150 ms` `misForced=1` CWB on wake/settle and full ALS/proximity accuracy.

---

## 3. Telemetry, Logging & Bloat Daemon Containment (`cos17_thermal_once`)

### 3.1 `midasd`, `ostatsd`, `qvirtservice_rs` & Verbose Logging
- **Root Cause:**
  - `/system_ext/etc/init/midasd.rc` and `ostatsd.rc` launched `midasd` (20+ `obrain.pb` worker threads failing `OplusObrainClientInvoke` every 60 s) and `ostatsd` whenever `persist.sys.midas_ostats.restriction=0`, ignoring `midasd.enable=0`.
  - `/product/bin/vendor.qti.qvirt-service_rs` failed dynamic linking (`libcutils.so`) and crash-looped every `5 s` via `init`.
  - `my_product/build.prop` enabled full IMS/QXDM logging (`loglevel=255`, `rtp.enableqxdm=15`, `vt.enableadb=3`, `disable*Logs=0`), and `vendor` enabled `cnss_diag` and Wi-Fi `fulldump` / `interrupt_test_mode`.
- **Fix:**
  - `patches/03-telephony-wifi-rps/system_ext_{midasd,ostatsd}_rc.patch` gate `start` triggers on `enable=1`.
  - `src/init/cos17_daemons.rc` permanently stops `vendor.qvirtservice_rs` on `post-fs`, `boot_completed`, `running`, and `restarting`.
  - `patches/03-telephony-wifi-rps/system_init_rc_hygiene.patch` and `my_product_ims_audio_props.patch` silence all IMS/QXDM/Wi-Fi dump logs from `u:r:init:s0`.

### 3.2 Android 15 eBPF `FIREWALL_CHAIN_OEM_DENY_3` + `appops` Containment
- **Root Cause:** Live socket forensics (`ss -tupn`) revealed `com.oplus.location` (`uid=10199`, bound by `system_server`) running `WIFI_SCAN`, `READ_CELL_INFO`, holding `sns_smd Wakeup`, and keeping 5 concurrent TCP sockets over 5G/LTE `rmnet_data2` to Baidu/China Unicom endpoints with stuck `Send-Q` TCP retransmissions — preventing the 5G RF transceiver (`sdr0 = 43 °C`) and PA (`44.3 °C`) from entering `RRC_IDLE` even with screen OFF.
- **Fix (`src/daemons/cos17_thermal_once.c`):**
  - Enables Android 15's kernel eBPF chain (`cmd connectivity set-chain3-enabled true`) and blocks background networking (`OEM_DENY_3`) + background `appops` on non-UID-1000 CN telemetry packages (`com.oplus.location`, `com.oplus.locationproxy`, `com.oplus.framework.network.drs`, `com.coloros.sceneservice`, `com.oplus.pantanal.ums`, `com.oplus.metis`, `com.nearme.instant.card`, `cn.richinfo.dmyk`, `com.oppo.ctautoregist`, `com.opos.ads`). Result: **`sdr0` powered down from `43.0 °C` -> `-273.0 °C (OFF)`** at idle!
  - Explicitly keeps **`com.heytap.mydevices`** in the `active` standby bucket (`bucket=5`) with `RUN_ANY_IN_BACKGROUND` + `WAKE_LOCK` allowed so Bluetooth connected-device UI works normally.
  - Configures **`com.oplus.aiunit`**, **`com.oplus.deepthinker`**, and **`com.heytap.market`** for on-demand usage (`0.0%` CPU when closed, full Gallery AI & App Market functionality when opened).

### 3.3 Native ELF Culprits (`libcameraopt`, `citsensorservice`, `libatlasservice`, `horae`, `sensors.qsh`, `libopex`, `AudioEffectCenter`)
- **`/vendor/lib64/libcameraopt.so`:** Spawned `traceCgroupWorker` (`WatchDog::startMonitor`) reading `/dev/cpuctl/*/cpu.shares` 24/7, triggering **21,163+ SELinux AVCs in 30 minutes** (`15,977+` kernel `audit_lost`). Fixed via `scripts/patch_pass2_culprits.py` (`0` AVCs, camera PerfLock boosts intact).
- **`/odm/bin/hw/vendor.xiaomi.sensor.citsensorservice.aidl`:**
  1. Looped at `1 Hz` 24/7 calling `waitForService("IHypSysSsi/default")` (missing HyperOS service), burning `7,780` jiffies in `init` + `828` in `servicemanager`.
  2. Subscribed 24/7 to `0x00000868` (`oem_collision_detection`) as a **`200 Hz` WAKEUP sensor**.
  3. Flooded `logd` at `48 Hz` (`808` lines / 15 s) from `libsensor-boledalgo` and `libsensor-qshcalapi`.
  Fixed via `scripts/patch_pass2_culprits.py` while keeping `TCS3720` ALS/proximity 100% functional.
- **`/system_ext/lib64/libatlasservice.so` & `/system_ext/bin/horae`:** `libatlasservice.so` ran a `1 Hz` retry thread against missing Oplus GPU minidump sysfs, while `horae` repeatedly failed `writeNode("/proc/shell-temp", ...)` every `5 s` (missing on Xiaomi kernel, spamming `E horae` logs). Fixed cleanly via `scripts/patch_pass2_culprits.py`.
- **`/vendor/lib64/sensors.qsh.so`:** OPPO `OplusExSystemService` requested sensor type `0x01fa2672` (OPPO MagSafe Hall sensor), colliding with Xiaomi 15's `tcs3408 back CCT` rear color sensor and polling it at `5 Hz` 24/7 even screen-off. Fixed via `scripts/patch_sensors_qsh_cct.py` (`0x1fa2672 -> 0x1fa2692`) without breaking OPPO APK signatures.
- **`/my_product/build.prop` (`AudioEffectCenter`):** `ro.oplus.audio.effect.type=audiox_merge` caused `AEC.AudioxService` to fail `AudioFlinger::createEffect` and throw a Java `RuntimeException` every `3 s` 24/7. Switching to `ro.oplus.audio.effect.type=dolby` matches Xiaomi 15's Dolby Atmos HAL (`0` retries).
- **`/system_ext/lib64/libopex.so`:** Unpacking the 6 `/system_ext/opex/*.opex` modules statically and applying `scripts/patch_libopex.py` eliminates 6 runtime loop + overlayfs mounts at boot.

---

## 4. Memory Management, Post-Setup Dexopt & Setup Wizard Haptics

### 4.1 `kswapd0` Page-Reclaim Thrashing (`vm.swappiness=60`, `watermark_scale_factor=20`)
- **Root Cause:** Stock `vendor/bin/init.kernel.post_boot-memory.sh` set `vm.swappiness=100` and `vm.watermark_scale_factor=65`. On 12 GB RAM with ColorOS memory load, `kswapd0` thrashed continuously for **`20.72 s` CPU time** during a 4-minute scroll test even with `>6 GB` RAM free.
- **Fix:** `patches/04-memory-vm-props/vendor_post_boot_memory_watermark.patch` lowers `WatermarkScale=65 -> 20`, reinforced by `src/init/init.cos17_dada_perf.rc` (`swappiness=60`) and `src/daemons/cos17_thermal_once.c` -> **`kswapd0` CPU dropped `20.72 s -> 0.00 s`**.

### 4.2 Setup-Wizard-Safe Background Dexopt & Haptics Overlap Fix
- **Root Cause:**
  1. Running `cmd package bg-dexopt-job` immediately on `sys.boot_completed=1` spiked CPU & battery temperature during first-boot Setup Wizard (`OplusBootReg`).
  2. During Setup Wizard, ColorOS RichTap cancels active HE playback via `IOplusVibrator::updateHeParameter` (`code=6`) with `(0, 0, 0)`, and multi-burst waveforms overlapped on rapid touches.
- **Fix:**
  - `src/daemons/cos17_dexopt_once.c` checks `settings get global device_provisioned` once (exiting immediately with zero loop during Setup Wizard), and on the first cool boot after setup waits `120 s`, pins itself to cores `0..5` with `nice(19)`, and checks `board_sensor_temp < 38.0 °C` before compiling each package.
  - `src/daemons/cos17_memkiller_ctl.c` replaces a `350 ms` shell polling loop with a pure-C one-shot `lmkd` helper.
  - `src/haptics/xmvibproxy_v2.cpp` intercepts `code=6 (0, 0, 0)` to cancel active HE playback immediately and maps continuous HE events strictly to verified single-envelope Xiaomi 15 RTP waveforms (`0.0%` idle CPU).

---

## 5. Modem, Wi-Fi & Network Interrupt Offload

### 5.1 Dynamic `rps_cpus=3c` Steering (`-90.2%` `CPU0 NET_RX`)
- **Root Cause:** `rmnet_data*` defaulted to `rps_cpus=3d` (which includes `CPU0`, bit 0!), while dynamically created `rndis0` (USB tethering) and `wlan2` (SoftAP hotspot) defaulted to `00` (all `NET_RX` softirqs piling onto `CPU0`, waking the primary scheduler core and raising `CPU0` frequency).
- **Fix:** `src/init/init.cos17_dada_perf.rc` steers `rmnet_data0..7`, `rndis0`/`ncm0`/`usb0`, and `wlan0`/`wlan2` (`rx-0..4`) to **`rps_cpus=3c`** (`CPU2..5`) across boot, USB gadget, Wi-Fi driver, and SoftAP property transitions, reducing `CPU0 NET_RX` softirqs by **`-90.2%`** (`8,102 -> 798`).

### 5.2 WCN7851 Hardware ARP & APFv6 Offload + RSSI Polling Interval
- **Root Cause:** Stock `WCNSS_qcom_cfg.ini` included CTS debug workarounds `hostArpOffload=0` and `gApfv6DisableOffloadBitmap=0x0`, forcing the AP CPU to wake up for routine ARP/multicast packets instead of handling them in WCN7851 firmware. Meanwhile, ColorOS Wi-Fi polled RSSI every `2000 ms`.
- **Fix:**
  - `patches/03-telephony-wifi-rps/vendor_wcnss_qcom_cfg.patch` removes `hostArpOffload=0` and `gApfv6DisableOffloadBitmap=0x0` and enables `gEnableNUDFailureNotify=4` (matching ColorOS 16 Beta 3).
  - `src/daemons/cos17_thermal_once.c` sets `cmd wifi set-poll-rssi-interval-msecs 6000` (`6000 / 10000 ms`), enabled rootlessly via `(allow cos17_memkiller wifi_service (service_manager (find)))` in `patches/03-telephony-wifi-rps/system_sepolicy_cos17_snippet.cil`.

### 5.3 5G SA Cell-Search Drain (`default_network=33,33`, `default_sa=false`)
- **Root Cause:** Base port set `ro.telephony.default_network=26,26` with 5G Standalone (`SA`) hunting enabled, keeping modem RF active on non-SA networks.
- **Fix:** Restore Stock Xiaomi 15 `ro.telephony.default_network=33,33` and `persist.sys.oplus.radio.default_sa=false`, eliminating futile SA cell-search drain while preserving full **5G NSA (EN-DC)** and **VoLTE**.

---

## 6. UI Compositor Efficiency (4K60 Video Backdrop-Blur Suppression)

- **Root Cause:** In Titanium Browser's ColorOS 17 Liquid Glass UI, attaching `SurfaceControl` blur effect layers to *every* Chromium `SurfaceView` created **6 double-stacked Kawase GPU blur layers** (`GpuWorkeBPF system = 89.8%`). Keeping those layers active at `60 fps` over solid black letterboxes during 1080p60/4K60 HW video forced `SurfaceFlinger` RenderEngine into `CLIENT` GPU composition every single frame.
- **Fix (`src/ui/VideoBlurSuppressionController.java`):**
  1. Lock `SurfaceControl` blur strictly to **one** primary `SurfaceView` (largest area, topmost Z) and lazily allocate `SurfaceControl.Transaction` only on state transitions.
  2. Immediately clear `backgroundBlurRadius=0` and hide effect layers (`O(1)`) on direct media URLs, fullscreen, Tab Switcher, or debounced embedded `<video>` playback (`FLAG_KEEP_SCREEN_ON` / `View.getKeepScreenOn()` / `AudioManager.isMusicActive()`).
  3. Result: `hwc_client` drops to **`0`** (zero-GPU `DEVICE` HWC overlay), cutting 4K60 H.264 `SurfaceFlinger` CPU by **`-82.5%`** (`251j -> 44j`) and battery power by **`-1,201 mW` (`-31.4%`)**.

---

## 7. Companion GKI Kernel (`6.6.118-android15-wild-trsy` `r24`)

See [`docs/KERNEL-GKI-TWEAKS.md`](docs/KERNEL-GKI-TWEAKS.md) for the exact KMI/ABI-safe defconfig fragment (`CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y`, `# CONFIG_RCU_LAZY_DEFAULT_OFF is not set`) and runtime verification details.

---

## Critical Anti-Patterns & Warnings for Porters

| Anti-Pattern | What Breaks If You Do It | Correct Solution |
| :--- | :--- | :--- |
| **`stop vendor.diag-router`** on SM8750 (`sun`) | Qualcomm `libdiag.so` enters a **100% CPU `error:111` busy-loop** when its socket closes. | Leave `vendor.diag-router` running; stop only `vendor.cnss_diag` and silence QXDM/IMS log properties. |
| **Adding UID-1000 packages to `OEM_DENY_3`** (`com.heytap.mcs`, `com.oplus.obrain`, `com.oplus.midas`, `com.oplus.onetrace`) | eBPF `OEM_DENY_3` keys on **UID**, not package name — blocking UID `1000` breaks `system_server` & framework networking! | Restrict UID-1000 packages via per-package **`appops` only**; reserve `OEM_DENY_3` strictly for non-UID-1000 packages (`uid >= 10000`). |
| **Putting `system_prop` / `vendor_ims_prop` or `watermark_scale_factor` in `vendor_init`** | Enforcing SELinux denies `vendor_init` from setting `persist.sys.*`, `persist.oplus.*`, `persist.vendor.ims.*`, or writing `proc_watermark_scale_factor` (silently reverting on clean flash!). | Place system/IMS triggers in `/system/etc/init/hw/init.rc` (`u:r:init:s0`) and write `WatermarkScale=20` from `vendor/bin/init.kernel.post_boot-memory.sh` + `cos17_thermal_once`. |
| **Re-signing OPPO platform APKs** (`OplusExSystemService.apk`, `AudioEffectCenter.apk`) | Breaks PackageManager shared-UID / platform signature verification. | Fix sensor collision in `/vendor/lib64/sensors.qsh.so` and audio type in `/my_product/build.prop` (`dolby`), leaving OPPO APKs 100% original. |
| **Adding `DirtySepolicy` rules** (`system_server execmem`, `zygote adb_data_file`, `dex2oat execute_no_trans`) | Fails AOSP neverallow & integrity checks. | Add strictly minimal domain-scoped CIL rules validated via `secilc -N` (`NEW_PAIRS=0`). |
