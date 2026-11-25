# Forensic Root-Cause Catalog (`ROOT-CAUSES.md`)

**Target Base Port:** ColorOS 17 V6 CN (`PLZ110_17.0.0.107(CN01)`, OnePlus 15T base, Android 17 / API 37) ported to Xiaomi 15 (`dada`, SM8750 / Stock HyperOS `OS4.0.0.11` vendor base).  
**Milestone:** **`v7.3.1`** (100% rootless-baked `super.img`, verified live in pure non-root `uid=2000` `SELinux Enforcing`).

This document catalogs every root cause identified through live thermal, CPU/GPU residency, softirq, SELinux audit, Binder, `framestats`, socket, and power-rail profiling, together with the exact non-destructive fix and critical anti-patterns to avoid.

---

## 1. Thermal HAL, Anti-Spike WALT & Perf Moderation (Zero Clock-Capping)

### 1.1 Unthrottled `mi_thermald` Scenario Runaway (`sconfig=50` / `sconfig=6` -> `sconfig=0`)
- **Root Cause:** ColorOS 17 left Xiaomi's `mi_thermald` in `sconfig=50` (`GL-PER-NORMAL` Performance Mode) or `sconfig=6` instead of Xiaomi 15's stock balanced `sconfig=0` (`GL-NORMAL`). Under `sconfig=50`, `[TEMP_AWARE]` dynamic thermal stepping is inactive, allowing board and CPU die temperatures to climb past `50 °C` during ordinary scrolling and screen-off 5G/hotspot operation.
- **Fix:** Write `0` to `/sys/class/thermal/thermal_message/sconfig` in `src/init/init.cos17_dada_perf.rc` (`sys.boot_completed=1`, `sys.boot_completed=1 && vendor.post_boot.parsed=1`, and `sys.user.0.ce_available=true`). Full `3.53 GHz` Medium and `4.32 GHz` Prime frequencies remain 100% uncapped in `cpufreq`.

### 1.2 `<PerfBreak/>` Thermal-Override Tables (`/odm/etc/` & `/vendor/etc/perf/`)
- **Root Cause:** ColorOS 17 (`PLZ110`) shipped active `<PerfBreak>` tables in both `/odm/etc/thermalbreakboostconfig.xml` and `/vendor/etc/perf/thermalbreakboostconfig.xml` (absent on ColorOS 16 Beta 3 and Stock HyperOS). On every UI scroll (`Hint="4224"`) and app launch (`Hint="4225"`, `"4226"`), `urcc`/`uah` overrode active thermal ceilings and pinned Medium cores to `2.40–2.78 GHz` and Prime cores to `2.25–4.09 GHz` even when board temperature was already `39–41 °C` (`50–53 °C` CPU die).
- **Fix:** Replace both tables with empty `<BoostConfigs><PerfBreak/></BoostConfigs>` (`patches/01-thermal-perf/vendor_thermalbreakboostconfig.patch` and `odm_thermalbreakboostconfig.xml`).

### 1.3 Inflated QTI PerfHAL Boost Floors & `SCHEDBOOST=2` Scroll Spikes (`vendor/etc/perf/perfboostsconfig.xml`)
- **Root Cause:**
  1. Base `perfboostsconfig.xml` pinned `MIN_FREQ = 0xFFF` on launch/animation boosts (`0x00001081`, `0x00001083`), disabled CPU deep-sleep LPM (`0x40408000`) during scrolls/flings/drags, forced Prime cores online, and applied a **5,000 ms `680 MHz` GPU floor** + `1500 MHz` BIG floor on `Large Comp Hint` (`0x00001097`).
  2. Live 50 ms traces in `v7.3` caught `Target="sun"` scroll/drag/fling hints (`0x00001080`, `0x00001087`, `0x000010A6`, `0x00001085`) injecting **`0x40C00000, 0x2` (`SCHEDBOOST=2`)**, slamming `policy0` straight to `2.23–2.40 GHz` (`63.7 °C` CPU die!) during trivial Settings scrolls even when GPU was idle at `222 MHz`.
- **Fix:** `patches/01-thermal-perf/vendor_perfboostsconfig.patch` (`v7.3.1`) strips `0x40C00000` (`SCHEDBOOST=2`) from all `Target="sun"` scroll/drag/fling hints, removes `min_freq = 0xFFF` and LPM disable opcodes, removes the 5-second `680 MHz` GPU floor on `0x00001097`, lowers touch (`0x1083`, `120 ms`) and status-bar (`0x11A0`, `300 ms`) floors to `1152/1209 MHz`, and lifts all residual `2745/4089` caps (`0x1086`/`0x1088`/`0x1089`) to `0xFFF` so `MAX_FREQ` (`3.53/4.32 GHz` CPU, `1,100 MHz` GPU) stays **100% uncapped**. `patches/01-thermal-perf/vendor_perfconfigstore.patch` enables `ro.vendor.perf.sagt=true` on `Target="sun"`.

### 1.4 Kernel 6.6 WALT & `/proc/sys/walt/input_boost/*` Sweet Spot (`v7.3`)
- **Root Cause:**
  1. Earlier init rules wrote legacy `/proc/sys/walt/input_boost_freq` instead of the real Android 15 / Kernel 6.6 sysfs paths under `/proc/sys/walt/input_boost/*`, leaving `powerkey_input_boost_freq = 2400000 / 2841600` (`400 ms`, `powerkey_sched_boost_on_input=1`) and `input_boost_ms=100` untouched.
  2. Vendor `init.kernel.post_boot-sun_default_6_2.sh` reset `up_rate_limit_us=0` and `sched_ed_boost=10` after early boot triggers, allowing single-frame load transients to spike `policy0` to `2.40 GHz` (`59.8–63.7 °C`).
  3. Over-damping `up_rate_limit_us=10000` (`10 ms`) exceeded a `120 Hz` frame budget (`8.33 ms`) and regressed Smart Sidebar `"All"` expansion jank from `2.52% -> 11.32%`.
- **Fix (`src/init/init.cos17_dada_perf.rc`):** Applied strictly on **`on property:sys.boot_completed=1 && property:vendor.post_boot.parsed=1`** AND re-asserted on **`on property:sys.user.0.ce_available=true`**:
  - `up_rate_limit_us=4000` (`4 ms` < half a 120 Hz frame), `down_rate_limit_us=0`, `hispeed_freq=960000` (`policy0`) / `1209600` (`policy6`), `hispeed_load=92 / 95`, `rtg_boost_freq=748800 / 1017600`, `boost=-15`, `pl=0`, `sched_ed_boost=0`, `sched_conservative_pl=1`, `display/cpu.uclamp.min=2`.
  - Real Kernel 6.6 input boost paths: `/proc/sys/walt/input_boost/input_boost_freq "960000 0 0 0 0 0 0 0"`, `input_boost_ms 60`, `powerkey_input_boost_freq "1363200 0 0 0 0 0 1689600 0"`, `powerkey_input_boost_ms 200`, `powerkey_sched_boost_on_input 0`.
  - **Result:** Simple 120 Hz scrolling CPU die dropped **`63.7 °C -> 47.8 °C` (`-13.9 °C`)** with **`0.23%` jank (`p99 = 12 ms`, `0` Missed Vsync, `0` Slow UI thread)**; Smart Sidebar `"All"` dropped **`63.3 °C -> 47.8–49.8 °C` (`1.63%` legacy jank, `p99 = 22 ms`)**.

### 1.5 Flattened Thermal FPS Table & Wrong Thermal Zone Path
- **Root Cause:** `/vendor/etc/display/thermallevel_to_fps.xml` flattened thermal levels `1..10` to `144 fps`, disabling Xiaomi 15's stock thermal FPS step-down (`144 -> 120 -> 90 -> 60 fps`), while `odm` & `my_stock` `sys_thermal_config.xml` enabled DCS/log uploads and pointed `<thermal_heat_path>` to missing `thermal_zone84` instead of Xiaomi 15's `thermal_zone57`.
- **Fix:** `patches/01-thermal-perf/vendor_thermallevel_to_fps.patch`, `odm_sys_thermal_config.patch`, and `my_stock_sys_thermal_config.patch`.

---

## 2. Eliminating Background Polling Loops & `SurfaceFlinger` Soft Reboots

### 2.1 `SurfaceFlinger` `RefreshRateSelector::setPolicy` `SIGABRT` Soft Reboots & Post-Doze Wake Trap (`v7.3`)
- **Root Cause:** Every mid-session userspace soft reboot (`tombstone_06..11`) originated from the exact same assertion inside `/system/bin/surfaceflinger` (`RefreshRateSelector::constructAvailableRefreshRates` at `+0x2cf190`):
  `No matching frame rate modes for primary range even after ignoring the render range. policy: {defaultModeId=3, allowGroupSwitching=false, primaryRanges={physical=[0.00 Hz, 120.00 Hz], render=[0.00 Hz, 60.00 Hz]}}`.
  Three bugs interacted:
  1. `LtpoFixHooks.findSfDisplayModeIdLocked()` synchronously remapped a `60 Hz` (`displayModeId=4`, `group=0`) mode into isolated `sfModeId=3` (`120.0000076 Hz`, `GROUP_IDLE_1` `0x1780100`) upon `STATE_DOZE` before `DisplayModeDirector` widened `DesiredDisplayModeSpecs`.
  2. Float conversion truncated `120.00f` (`8333333 ns` -> `120.000000f`) below HWC's `120.0000076f`, and `widenSpecsMax()` omitted updating `specs.vrrPolicy`.
  3. On wake from `STATE_DOZE` (`3 -> 2`), `BrightnessObserver.setDefaultDisplayState(2)` ran before `LocalDisplayDevice.mState` transitioned to `2`, temporarily trapping Quick Settings in `id=3` (`GROUP_IDLE_1`).
- **Fix (Two-Layer Defense):**
  - **Layer 1 (`src/ltpo/java/com/nyako/ltpofix/LtpoFixHooks.java` v8.1 + `src/ltpo/ltpofix_plugin.cpp` v8):** Removes unsynchronized `displayModeId != forceMode` hijacks, adds Hook 3 (`setDesiredDisplayModeSpecsLocked`) to always widen isolated modes to `120.1f` + sync `vrrPolicy`, tracks `sDirectorDisplayState`, and schedules `+120 ms` / `+320 ms` post-Doze wake resyncs (**`5/5` `id=1@120.00001Hz`** on Quick Settings pull).
  - **Layer 2 (`src/ltpo/overlay.cpp` v4.1):** Installs Trampoline 3 on `RefreshRateSelector::setPolicy` (`+0x2cdfac`) to clamp any narrow `DisplayManagerPolicy` for isolated 120 Hz modes (`sfModeId 1..3`) or locked 120 Hz policies (`0..5`) to `[0.0f, 120.1f]` (`8,326,394 ns`), making the `SIGABRT` soft reboot structurally impossible even with external refresh-rate modules active. Also sets `kIdle10Ms = 300 ms` with instant wake from `10 Hz` while preserving `0.000%` 1 Hz idle CPU (`0.06%` SF CPU).

### 2.2 `oplus-color-mode-sync` 2 Hz Fork Storm & `hdrpeakd` Fresh-Flash `98%` Spin Bug
- **`oplus-color-mode-sync`:** Spawned `/system/bin/settings` at `2 Hz` (`8,854` jiffies / 70 min). Fixed by `scripts/patch_surfaceflinger_renderintent.py` (in-process `301->257 / 303->258 / 307->256` RenderIntent translation) + `scripts/patch_color_mode_sync.py` (infinite kernel futex wait on `persist.sys.sf.native_mode` -> `0` jiffies).
- **`hdrpeakd`:** Legacy shell helper burned `10.82%` CPU; early C++ port re-zeroed `global_ser` inside a retry loop (`98%` CPU spin on fresh flashes). Fixed in `src/daemons/hdrpeakd.cpp` (`IHdrLayerInfoListener` + `IDisplayFeature` AIDL + once-seeded `__system_property_area_serial()` -> **`0.000%` idle CPU**).

### 2.3 Under-Display CWB Proximity/ALS Capture Rate (`ro.vendor.cwb.timeout=1000`)
- **Root Cause:** Xiaomi 15's BOLED TCS3720 library (`libclient2slpi.notifier.so`) triggered SurfaceFlinger CWB framebuffer captures every `200 ms` (`5 Hz`) during continuous display updates.
- **Fix:** `ro.vendor.cwb.timeout=1000` (`patches/02-display-ltpo-cwb/vendor_cwb_display_props.patch`) cuts steady-state CWB triggers by **`-79.5%`** (`39 -> 8` / 5 s) while preserving `150 ms` forced wake CWB and full ALS/proximity accuracy, paired with a `persist.vendor.debug.cwb.disable=false` dirty-flash guard in `init.cos17_dada_perf.rc`.

---

## 3. `v7.3.1` Idle CPU Breakthrough (`3.1–3.3% -> 0.79%`) & Background Churn Eliminations

### 3.1 `QtiImsExtConnector` Infinite Bind Storm (`11,734 -> 0` Hits, `-75%` Clean-Flash Idle CPU)
- **Root Cause:** On Xiaomi 15 ColorOS 17 V6 CN, `org.codeaurora.ims` is **not installed** (`imsdaemon` + `qcrilNrd` handle VoLTE/VoNR/5G EN-DC natively). Yet `product/framework/ims-ext-common.jar` (`org.codeaurora.ims.QtiImsExtConnector#bindQtiImsExtService()`) unconditionally invoked `mContext.bindService()` for `org.codeaurora.ims/.QtiImsExtService` and rescheduled a `500 ms` retry loop forever with zero backoff — generating **`11,734` `ActivityManager: Unable to start service ... not found` hits in 34 minutes** (`~6–10` Binder IPCs/s), keeping `com.android.phone` (`0.4%`), `system_server` (`2.2%`), and `logd` (`0.1%`) continuously awake and locking clean-flash idle CPU at `3.1–3.3%`.
- **Fix (`patches/06-stability-fcm-ims/product_ims_ext_common_qti_connector.smali.patch`):** Adds an early `PackageManager.resolveService(intent, 0) == null` check (`mBound = false; return;`) before `bindService()`, removes the 6 stale `product/framework/oat/{arm,arm64}/ims-ext-common.{odex,vdex,fsv_meta}` files, and syncs `packing/product/{fs_config,file_contexts}`.
- **Result:** **`11,734 -> 0`** hits; pure non-root screen-off `Asleep` idle CPU dropped from **`3.1–3.3%` -> `0.79%` (`-75%`)** while Dual-SIM LTE + 5G NR EN-DC + VoLTE (`imsdaemon`) remained 100% intact.

### 3.2 `init.oplus.debug.rc` 30s Shell Spawns & `netd.rc` `oplus_dnsproxyd` SELinux AVC Flood (`v7.3`)
- **`/system_ext/etc/init/init.oplus.debug.rc`:** `on property:debug.binderinfo.capture=true` forked `/system/system_ext/bin/autochmod.sh "binderinfocapture"` every `30 s` (`~150 ms` root shell + `SIGKILL`), while `catSuspendMode` exited with errors. `patches/03-telephony-wifi-rps/system_ext_init_oplus_debug_rc.patch` points both services to `/system/bin/true`, removes the `debug.binderinfo.capture` trigger, and adds core-init (`u:r:init:s0`) dirty-flash resets for CN radio props (`0` Treble AVCs).
- **`/system/etc/init/netd.rc` & `plat_file_contexts`:** `/dev/socket/oplus_dnsproxyd` lacked `u:object_r:dnsproxyd_socket:s0`, causing `gmscore_app` (`com.google.android.gms`) continuous `avc: denied` retries. Fixed by `system_netd_oplus_dnsproxyd.patch` + `system_plat_file_contexts_dnsproxyd.patch` (`0` GMS DNS AVCs, `NEW_PAIRS=0`).

### 3.3 `libnyako_core.so` Fork-Time Scan/AVC Storm (`122 -> 0` Post-Unlock Zygote Stalls)
- **Root Cause:** Every forked unprivileged/GMS/KSU app process probed `/data/local/nyako/state/scopes` (`-EACCES` SELinux `auditd` storm) and fell through into `nyako_scan_all_plugins()` (>10 MB reads across 24 `.so` libraries), while non-UID-0 `system_app`s (`Camera`, `PhoneManager`) invoked `chmod()` on `root:root` dirs (`avc: denied { setattr }`), and `fsetxattr(nyako_data)` returned hard errors under Enforcing SELinux. Combined with `cos17_thermal_once`'s earlier 347 boot-time spawns, this caused **`122` post-unlock `ActivityManager: Slow operation ... startProcess: returned from zygote!` stalls (`2.3–2.6 s` each)**.
- **Fix (`src/nyako/nyako_scope_filter.c` + `scripts/patch_libnyako_scope.py`):** Embeds a freestanding zero-relocation 17-scope PC-relative matcher at `0x190800` inside `PT_LOAD[4]`, gates `mkdir`/`chmod` on `geteuid() == 0`, and replaces `fsetxattr@plt` calls with `mov w0, wzr`. Result: **`122 -> 0` stalls**, `nyako_avc = 0`, `64/64` pages (`256 KiB`) static RX pool free.

---

## 4. Active 5G & Idle Radio Thermal Fixes (`39–40 °C -> ~36–37.2 °C`, `pa_therm0 -17.2 °C`)

### 4.1 Legacy 3G-Only Fast Dormancy Mask (`0x028708` -> `0x1AA708`)
- **Root Cause:** Although `vendor.dpmd` ran with `persist.vendor.dpm.vndr.feature=1`, `/vendor/etc/dpm_vndr/vendor.dpm.conf` and `/system_ext/etc/dpm/dpm.conf` carried legacy 3G-only mask `0x028708`, leaving 4G LTE (`bit 13`), LTE_CA (`bit 19`), and 5G NR (`bit 20`) Fast Dormancy disabled (`0` `FD_*` timers in `/sys/class/xt_idletimer/timers/`).
- **Fix:** `vendor_dpm_fast_dormancy.patch` & `system_ext_dpm_fast_dormancy.patch` set `0x1AA708` with `5 s` screen-on / `3 s` screen-off / `15 s` tethering-on timers -> `/sys/class/xt_idletimer/timers/FD_rmnet_data1` and `FD_rmnet_data2` activate natively and transition the 5G RF transceiver (`sdr0 -> -273.0 °C OFF`) & PA (`pa_therm0 58.7 °C -> 40.8–41.5 °C`) to `RRC_IDLE`.

### 4.2 `UID 1000` `com.heytap.mcs` Stuck TCP Retransmits & Volatile `OEM_DENY_3` Reset
- **Root Cause:**
  1. `ss -tupn` proved `com.heytap.mcs` (`UID 1000`, immune to UID-keyed `OEM_DENY_3`) kept stuck HTTPS retransmit queues (`Send-Q = 463–1243`, `45 TX pkts / 15 s` = `3 pkts/s`) to `httpdns-push-cn.heytapmobi.com`, continuously resetting `FD_rmnet_data2` and locking `pa_therm0` up to `58.7 °C`.
  2. Android's kernel eBPF `FIREWALL_CHAIN_OEM_DENY_3` map is **volatile across reboots** — gating it behind a one-time version property caused `com.oplus.metis`, `com.oplus.pantanal.ums`, and `com.oplus.cosa` to revert to `allow` on normal reboots.
  3. UID-mode `MONITOR_LOCATION: allow (running)` on `com.oplus.metis` / `com.oplus.pantanal.ums` fired `GET_CELL_INFO_LIST` modem polls whenever Wi-Fi was off.
- **Fix:**
  - `patches/03-telephony-wifi-rps/system_hosts_cn_telemetry_sinkhole.patch` sinkholes 31 verified CN push/HTTPDNS/DCS telemetry domains to `127.0.0.1`/`::1` (instant local `ECONNREFUSED`, `0` radio packets; App Market, Gallery AI, MyDevices, and OShare untouched).
  - `src/daemons/cos17_thermal_once.c` (v8, `HYGIENE_VER=75`): re-applies the 14 verified non-1000 CN package `OEM_DENY_3` entries on **every boot**, gates the 350+ command persisted AppOps pass (adding `--uid` mode strictly on verified non-1000 UIDs, keeping UID-1000 `com.oplus.onetrace` package-scoped, disabling `McsService`, setting `dual_ps_enable_state=0`, and keeping `com.heytap.market` 100% unrestricted) behind `HYGIENE_VER=75` with `nice(19)` + `20 s` post-CE-unlock settle, and binds all 3 stdio FDs to `/dev/null` (`0` Binder AVCs).

### 4.3 CN 5G NSA / Dual-5G SA / RegionLock Props & `rmnet_ipa0` RPS Offload
- **Root Cause:** `my_product/build.prop` set `persist.oplus.radio.not_support_nsa_hw_ver=All`, `not_support_nsa_show_5g=1`, `dual_nr_enabled=true`, `sub_slot_support_sa=1`, `checkservice=true`, and `global_regionlock.allcheck=true` (plus `global_regionlock.enabled=true` in `cust/CN/build.prop`), conflicting with global EN-DC 5G NSA networks and keeping Dual-SIM SA search active. Meanwhile, `rmnet_ipa0` defaulted to single-core `rx-0=02`, `rx-1=00`, `rmnet_data*` to `3d` (including `CPU0`), and `rndis0`/`wlan2` to `00`.
- **Fix:** `my_product_ims_audio_props.patch`, `my_product_cust_cn_regionlock.patch`, `system_ext_init_oplus_debug_rc.patch` (`-1` sentinel verified in `OplusNrStateUpdater` bytecode), and `src/init/init.cos17_dada_perf.rc` steering `rmnet_ipa0/queues/rx-{0,1}`, `rmnet_data0..7`, `rndis0`/`ncm0`/`usb0`, and `wlan0`/`wlan2` to `rps_cpus=3c` (`-90.2%` `CPU0 NET_RX`).

### 4.4 WCN7851 Wi-Fi Hardware ARP/APFv6 Offload, `midasd`/`ostatsd`/`qvirtservice_rs` & Pass-2 Native ELF Culprits
- **Wi-Fi Offload:** `vendor_wcnss_qcom_cfg.patch` removes `hostArpOffload=0` / `gApfv6DisableOffloadBitmap=0x0` and sets `gEnableNUDFailureNotify=4`; `cos17_thermal_once` sets Wi-Fi RSSI polling to `6000 / 10000 ms` via the `wifi_service` SELinux CIL rule (`NEW_PAIRS=0`).
- **Telemetry Daemons & Logs:** `system_ext_{midasd,ostatsd}_rc.patch`, `cos17_daemons.rc` (`stop vendor.qvirtservice_rs`), `system_init_rc_hygiene.patch`, and `my_product_ims_audio_props.patch` (IMS/QXDM silence + `audiox_merge -> dolby`).
- **Pass-2 ELF Culprits:** `scripts/patch_pass2_culprits.py` (`libcameraopt.so` `21,163 -> 0` AVCs; `citsensorservice.aidl` `1 Hz` missing `IHypSysSsi` loop + `200 Hz` collision sensor + `48 Hz` logd spam; `libatlasservice.so`; `horae`), `scripts/patch_sensors_qsh_cct.py` (`tcs3408` Hall sensor collision `0x1fa2672 -> 0x1fa2692`), and `scripts/patch_libopex.py` (eliminate 6 boot loop+overlayfs mounts).

---

## 5. Memory VM (`swappiness=60`, `wsf=20`), Post-CE ART Dexopt & Setup Wizard Haptics

### 5.1 `kswapd0` Reclaim Thrashing & `cos17_daemons.rc` `swappiness=100` Conflict (`v7.3.1` Fix #2)
- **Root Cause:** Stock `vendor/bin/init.kernel.post_boot-memory.sh` set `vm.swappiness=100` and `WatermarkScale=65` (`kswapd0` burned `20.72 s` during a 4-min scroll test). In `v7.3`, a clean-flash forensic audit (`HEAT-CHECK.md`) caught a subtle secondary override: `vendor/etc/init/cos17_daemons.rc` line 52 still wrote `write /proc/sys/vm/swappiness 100` when `persist.sys.oplus.disable_mem_killer=0`, overriding `init.cos17_dada_perf.rc` and causing `1,746,708` boot-time swap-outs (`kswapd0` `56 s` CPU) with `6.82 GB` RAM available!
- **Fix:** `patches/04-memory-vm-props/vendor_post_boot_memory_watermark.patch` (`WatermarkScale=20`) + `src/init/cos17_daemons.rc` (`v7.3.1`: `disable_mem_killer=0` now writes **`swappiness 60`**, keeping `disable_mem_killer=1` Developer Options toggle intact) -> **`kswapd` settled delta `0` ticks (`pgscan=0`, `pswpout=0`)**.

### 5.2 Post-CE-Unlock ART Compilation (`SystemUI.apk.prof` CRC Mismatch & `SmartSideBar` `verify`)
- **Root Cause:** Early `cos17_dexopt_once` wrote `/data/system/.cos17_dexopt_done` (`-EACCES` under `cos17_memkiller`) and ran pre-unlock while `/data/misc/profiles/cur/0/*` was FBE-encrypted, leaving patched `SystemUI.apk` (`95 MB`) and `Settings.apk` (`200 MB`) in **`[status=run-from-apk]`** and `SmartSideBar.apk` in `[status=verify]` (`SystemUI` `Slow UI thread = 166`, `p99 = 61 ms`). Even with `-m speed-profile`, `SystemUI.apk.prof` had mismatched DEX CRCs on `classes3.dex` & `classes4.dex` (only `23 MB` `.odex` / `7/9` DEXes).
- **Fix (`src/daemons/cos17_dexopt_once.c` v2, `DEXOPT_VER=75`):** Waits for `sys.user.0.ce_available=true` + `25 s` `nice(19)` settle, binds all 3 stdio FDs to `/dev/null` (`0` AVCs), compiles `com.android.systemui` (`101 MB` `.odex`, `9/9` DEXes) and `com.coloros.smartsidebar` with **`-m speed`** and 8 other core ROM packages with `-m speed-profile`, and gates via `persist.sys.cos17.dexopt_ver=75` (`0.001 s` skip on subsequent boots).

### 5.3 Setup Wizard Haptics Overlap Fix (`xmvibproxy_v2.cpp`) & `cos17_memkiller_ctl.c`
- `src/haptics/xmvibproxy_v2.cpp` intercepts `IOplusVibrator::updateHeParameter` (`code=6`, `(0, 0, 0)`) to stop active HE playback cleanly and maps continuous HE events to single-envelope Xiaomi 15 RTP waveforms (`0.0%` idle CPU).
- `src/daemons/cos17_memkiller_ctl.c` replaces legacy `350 ms` shell polling with a pure-C one-shot `lmkd` helper.

---

## 6. Rootless Zero-Delay FCM Push Architecture (`v7.3`)

- **Root Cause:** ColorOS 17 CN breaks/delays Google FCM push notifications across **five** independent gates:
  1. `<feature name="cn.google.services" />` declared in **both** `/my_product/etc/permissions/oplus_google_cn_gms_features.xml` **and** Xiaomi CN `/odm/etc/permissions/com.gnss.bds_preference.xml`.
  2. `com.google.android.gms` missing from `/my_region/etc/battery/sys_deviceidle_whitelist.xml`.
  3. `com.oplus.battery` (`GoogleRestrictionController` + `OplusNetworkingControlManager` + `ControllerCenter`) applying `POLICY_REJECT_ALL` and broadcasting `oplus.intent.action.google_restrict_change` (`restrict_enable=true`).
  4. `system_server` (`OplusBgSceneManager`, `OplusDeviceIdleHelper`, `OplusGoogleAlarmRestrict`, `OplusDeepSleepHelper`, `OplusNetworkManagementService`, `OAppNetControlService`, `OsenseResManagerService`) downgrading GMS wakeup alarms (`type 0/2 -> 1/3`), blocking GMS network/wakelocks, and cleaning GMS in deep sleep.
  5. `OplusProxyBroadcast` / `BroadcastProxyAction` / `OplusAppStartupManager` / `HansCGroup` proxying or blocking `com.google.android.c2dm.intent.RECEIVE` and refusing to wake force-stopped apps.
- **Fix:** `patches/06-stability-fcm-ims/{my_product_cn_gms_features,odm_gnss_bds_preference,my_region_deviceidle_whitelist}.patch` + split-scope `src/fcm/` (`libNyako_hook_fcmsys.so` `scope="android"` 15 hooks + `libNyako_hook_fcmbatt.so` `scope="com.oplus.battery"` 3 hooks; leak-free bounded 32-slot `20 s` ring buffer, zero threads).
- **Verified Result:** Inside forced `Deep Doze=IDLE`, `mtalk.google.com:5228` remains `ESTAB` (`4–6 ms` heartbeats), GMS `c2dm.intent.RECEIVE` flag rewrites `0x400010 -> 0x30` in **`+3 ms`**, and a force-stopped receiver cold-starts in **`+18 ms`** — working identically both standalone (pure non-root clean flash) and alongside optional LSPosed `fcmfix-coloros17`.

---

## 7. UI Compositor Efficiency & Companion GKI Kernel

- **Titanium Browser 4K60 Video Backdrop-Blur Suppression (`src/ui/VideoBlurSuppressionController.java`):** Locks `SurfaceControl` blur to one primary `SurfaceView` and clears `backgroundBlurRadius=0` (`O(1)`) during video playback/fullscreen/Tab Switcher -> `hwc_client = 0`, 4K60 H.264 `SurfaceFlinger` CPU **`-82.5%`** (`251j -> 44j`), battery power **`-1,201 mW` (`-31.4%`)**.
- **Companion GKI Kernel (`6.6.118-android15-wild-trsy` `r24`):** See [`docs/KERNEL-GKI-TWEAKS.md`](KERNEL-GKI-TWEAKS.md) (`CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y`, `# CONFIG_RCU_LAZY_DEFAULT_OFF is not set`, `0` KMI symbols changed).

---

## 8. Critical Anti-Patterns & Thermal Boomerang Lessons Learned

| Anti-Pattern | What Breaks / Regresses If You Do It | Correct Verified Solution |
| :--- | :--- | :--- |
| **`stop vendor.diag-router`** on SM8750 (`sun`) | Qualcomm `libdiag.so` enters a **100% CPU `error:111` busy-loop** when its socket closes. | Leave `vendor.diag-router` running; stop only `vendor.cnss_diag` and silence QXDM/IMS log properties. |
| **`persist.vendor.debug.cwb.disable=true`** on Xiaomi 15 (`dada`) | Breaks Xiaomi 15's under-display BOLED TCS3720 ALS/Auto-Brightness and optical proximity readings. | Throttle steady-state CWB with `ro.vendor.cwb.timeout=1000` (`-79.5%` captures, 100% ALS accuracy) and enforce `cwb.disable=false` dirty-flash reset. |
| **Dropping raw Stock `OS4.0.0.11` `perfboostsconfig.xml` directly** | Re-introduces a **`5000 ms` `680 MHz` GPU floor** (`0x00001097`) and `2745/4089` boost floors/caps -> **`+0.9 °C` battery boomerang in 75 s**. | Apply `patches/01-thermal-perf/vendor_perfboostsconfig.patch` (`v7.3.1`: zero scroll `SCHEDBOOST=2`, zero GPU floor, zero CPU/GPU max-clock caps). |
| **Setting `up_rate_limit_us=10000` (`10 ms`) on 120 Hz display** | `10 ms > 8.33 ms` frame budget delays frequency ramp past one Vsync -> regresses Smart Sidebar `"All"` jank **`2.52% -> 11.32%`**. | Use **`up_rate_limit_us=4000` (`4 ms`) + `input_boost_freq=960000` (`60 ms`) + `sched_ed_boost=0`** (`0.23%` scroll jank at `47.8 °C`). |
| **Adding UID-1000 packages to `OEM_DENY_3` or `--uid` `appops`** | eBPF `OEM_DENY_3` and `appops --uid` key on **UID** — blocking UID `1000` breaks `system_server` networking & permissions system-wide! | Restrict UID-1000 packages (`com.heytap.mcs`, `com.oplus.onetrace`, `obrain`, `midas`) via **package-mode `appops` only** + `/system/etc/hosts`; re-apply non-1000 `OEM_DENY_3` every boot (kernel BPF map is volatile). |
| **Leaving `swappiness 100` in `cos17_daemons.rc` (`disable_mem_killer=0`)** | Silently overrides `swappiness=60` on clean flash -> **`1.74M` boot-time swap-outs** (`kswapd0` `56 s` CPU) with `6.8 GB` RAM free. | Set `write /proc/sys/vm/swappiness 60` in `disable_mem_killer=0` (`src/init/cos17_daemons.rc`). |
| **Leaving stale `oat/{arm,arm64}/ims-ext-common.*` after patching `ims-ext-common.jar`** | ART loads stale unpatched `.odex`/`.vdex` or fails `.fsv_meta` verification, keeping the `QtiImsExtConnector` bind storm alive. | Delete all 6 `product/framework/oat/{arm,arm64}/ims-ext-common.{odex,vdex,fsv_meta}` files and update `packing/product/{fs_config,file_contexts}`. |
| **Putting `system_prop` / `vendor_ims_prop` / `watermark_scale_factor` in `vendor_init`** | Enforcing SELinux denies `vendor_init` from setting `persist.sys.*`, `persist.oplus.*`, `persist.vendor.ims.*`, or writing `proc_watermark_scale_factor`. | Place system/radio/IMS triggers in `/system/etc/init/hw/init.rc` or `/system_ext/etc/init/init.oplus.debug.rc` (`u:r:init:s0`) and write `WatermarkScale=20` in `init.kernel.post_boot-memory.sh` + `cos17_thermal_once`. |
| **Re-signing OPPO/Qualcomm shared-UID platform APKs** (`OplusExSystemService`, `AudioEffectCenter`, `CneApp`) | Breaks PackageManager shared-UID / signature checks on rootless dirty flashes (`INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES`). | Fix sensor collision in `sensors.qsh.so` and audio type in `my_product/build.prop` (`dolby`), leaving shared-UID OEM APKs 100% original. |
| **Adding `DirtySepolicy` rules** | Fails AOSP neverallow & integrity checks. | Add strictly minimal domain-scoped CIL rules validated via `secilc -N` (`NEW_PAIRS=0`). |
