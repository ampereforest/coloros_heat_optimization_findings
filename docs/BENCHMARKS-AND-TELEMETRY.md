# Benchmarks & Live Telemetry Ledger (`BENCHMARKS-AND-TELEMETRY.md`)

All measurements below were captured on a **bare Xiaomi 15 (`dada`, SM8750, 12 GB RAM)** with **zero external cooling fan**, running **ColorOS 17 V6 CN (`PLZ110_17.0.0.107(CN01)`, Android 17 / API 37)** across Passes 1–3, `v7.1`, `v7.2`, `v7.3`, and the final pure non-root **`v7.3.1`** clean flash + **`6.6.118-android15-wild-trsy` (`r24`)** kernel.

---

## 1. Pass 1 & Pass 2 — Background Daemon, AVC & Logcat Storm Elimination

Measured over 15-second active/idle windows and a 70-minute settled run:

| Culprit / Metric | BEFORE | AFTER (Pass 1 + Pass 2) | Change |
| :--- | :---: | :---: | :---: |
| `/odm/bin/hw/oplus-color-mode-sync` (70 min) | `8,854` jiffies (`2 Hz` `/system/bin/settings` fork) | **`0` jiffies** | **`-100%`** |
| `/vendor/lib64/libcameraopt.so` SELinux AVCs (30 min) | `21,163+` denials (`15,977+` `audit_lost`, `5,205j`) | **`0` denials** (`0` `audit_lost`) | **`-100%`** |
| `citsensorservice.aidl` missing `IHypSysSsi` 1 Hz loop | `7,780j` (`init`) + `828j` (`servicemanager`) | **`0` retries** (`86,400 s` condvar sleep) | **`-100%`** |
| `citsensorservice.aidl` `0x00000868` collision sensor | `200 Hz` WAKEUP 24/7 | **Disabled** (`TCS3720` ALS 100% intact) | **`-100%`** |
| `libatlasservice.so` (`OplusGpuMinidump` missing sysfs) | `17` errors / 15 s (24/7) | **`0` errors** | **`-100%`** |
| `horae` (`writeNode("/proc/shell-temp")`) | Failing `open()` + `E horae` log every `5 s` 24/7 | **`0` syscalls / `0` errors** (`mov w0, #-1`) | **`-100%`** |
| `AudioEffectCenter` (`audiox_merge` -> `dolby`) | `54` lines + `RuntimeException` / 15 s (24/7) | **`0` errors** | **`-100%`** |
| `sensors.qsh.so` `tcs3408 back CCT` Hall collision | `5 Hz` active 24/7 + `bad parameter` spam | **`0` connections / `0` errors** | **`-100%`** |
| **15 s Logcat Rate (Screen-ON Idle)** | `1,581` lines | **`267` lines** | **`-83.1%`** |
| **15 s Logcat Rate (Screen-OFF Idle)** | `142+` lines | **`65` lines** | **`-54.2%`** |

---

## 2. Display, HDR Peak, LTPO & Cold Launch Efficiency

| Subsystem / Metric | BEFORE | AFTER | Change |
| :--- | :---: | :---: | :---: |
| **`hdrpeakd` CPU Load** | `10.82%` (shell) / `98%` (fresh-flash spin bug) | **`0.000%` idle** (AIDL + kernel futex) | **`-100%`** |
| **Hardware 1–120 Hz LTPO (`overlay.so` v4.1 + `pipe2`)** | `~4–6%` SF CPU (`10 Hz` `publishUni` + `1 MB` `memfd` poll) | **`0.06%` SF CPU at 1 Hz** (`0.000%` watchdog) | **`-98%`** |
| **180 s Home Screen Swipe — `policy0` Freq & GPU Floor** | GPU pinned `680 MHz` (`Large Comp Hint`), `policy0` `2.40–2.75 GHz` | `policy0` **`-34.4%`**, GPU **`222 MHz`**, battery rise **`3.5×` lower** | **`-34.4%` CPU** |
| **Post-Setup `cos17_dexopt_once` Cold Launch Latency** | Uncompiled post-wipe | Gallery **`-41.3%`**, Camera **`-23.3%`**, Settings **`-47.2%`** | **Up to `-47.2%`** |

---

## 3. UI Compositor — Titanium Browser Round 14 Video Playback & CWB Throttle

Measured on bare Xiaomi 15 (`dada`) during active hardware video decode (`c2.qti.avc.decoder` / `c2.qti.vp9.decoder`, `renderFps = 60`):

| Workload & Metric | BEFORE (`6` Blur Layers, `cwb=200ms`) | AFTER (`hwc_client=0`, `cwb=1000ms`) | Delta |
| :--- | :---: | :---: | :---: |
| **1080p60 H.264 (8 s) — HWC `CLIENT` Blur Layers** | `6` active Kawase GPU layers | **`0`** (`hwc_device = 8`) | **`-100%`** |
| **1080p60 H.264 (8 s) — `SurfaceFlinger` CPU** | `292` jiffies | **`121` jiffies** | **`-58.6%`** |
| **1080p60 H.264 (8 s) — `composer` HAL CPU** | `82` jiffies | **`58` jiffies** | **`-29.3%`** |
| **4K60 H.264 40 Mbps (5 s) — `SurfaceFlinger` CPU** | `251` jiffies | **`44` jiffies** | **`-82.5%`** |
| **4K60 H.264 40 Mbps (5 s) — Battery Power Draw** | `3,819 mW` | **`2,618 mW`** | **`-1,201 mW (-31.4%)`** |
| **4K60 H.264 40 Mbps (5 s) — `cpuss-0` Temp** | `67.5 °C` | **`65.2 °C`** | **`-2.3 °C`** |
| **4K60 H.264 (5 s) — Under-Display CWB Triggers** | `39` triggers (`200 ms`) | **`8` triggers** (`1000 ms`) | **`-79.5%`** |
| **4K60 VP9 15 Mbps (5 s) — `SurfaceFlinger` CPU** | `235` jiffies | **`46` jiffies** | **`-80.4%`** |
| **4K60 VP9 15 Mbps (5 s) — Battery Power Draw** | `2,439 mW` | **`2,141 mW`** | **`-298 mW (-12.2%)`** |
| **Tab Switcher Idle Redraws (2 s)** | Continuous 120 fps loop | **`0` frames in 2 s** (1 Hz LTPO intact) | **`-100%`** |

---

## 4. `v7.1` Controlled 2-Part Thermal A/B Benchmark

### Part A — 180 s Screen-OFF Cooldown (5G/LTE Active + `wlan2` Hotspot + `rndis0` USB Tethering)

| Metric | BEFORE (`sconfig=50`, `PerfBreak` active, `hostArpOffload=0`, `rmnet=3d`, `wsf=65`) | AFTER (`sconfig=0`, `<PerfBreak/>`, COS16 Wi-Fi offload, `rps=3c`, `wsf=20`) | Delta |
| :--- | :---: | :---: | :---: |
| **Battery Power Draw (Idle Unplugged)** | `3,362 – 3,946 mW` | **`402 – 984 mW`** | **`-81%`** |
| **Battery Temp (`t = 180 s`)** | `39.3 °C` | **`37.3 °C`** | **`-2.0 °C`** |
| **Samples with CPU Zone ≥ 50 °C (`GT50`)** | `2 / 13` samples | **`0 / 13` samples** | **`-100%`** |
| **PMIC (`pmih010x_lite_tz`) Drift (Hotspot Load)** | Climbed `48.89 -> 50.40 °C` (`+1.51 °C`) | Held **`47.61 – 48.75 °C`** (`0/13 ≥ 50 °C`, `-1.65 °C` at `180 s`) | **Cooldown restored** |
| **5G RF Transceiver (`sdr0`, eBPF `OEM_DENY_3`)** | `42.0 – 43.0 °C` (stuck `RRC_CONNECTED` via CN TCP retransmits) | **`-273.0 °C` (RF OFF)** with 5G/LTE & Wi-Fi enabled | **True RF idle** |

### Part B — 240 s Real-World 120 Hz Light Usage (120 s Homescreen L/R Scroll + 120 s Settings U/D Scroll)

| Metric | BEFORE | AFTER | Delta |
| :--- | :---: | :---: | :---: |
| **Battery Temp (`t = 240 s`)** | `40.3 °C` | **`38.2 °C`** | **`-2.1 °C`** |
| **Settings Scroll — Avg CPU Zones ≥ 50 °C (`GT50`)** | `13.1` zones/sample (`54.4 °C` peak) | **`2.25` zones/sample** | **`-83%`** |
| **`kswapd0` Cumulative CPU Time** | `20.72 s` | **`0.00 s`** | **`-100%`** |
| **`CPU0` `NET_RX` Softirqs** | `8,102` | **`798`** | **`-90.2%`** |
| **Medium Cluster (`policy0`) Residency at Min `556.8 MHz`** | `141,390 ms` (`53.5%`) | **`154,980 ms` (`58.7%`)** (`86.2% <= 1.15 GHz`) | **`+13.6 s` min-freq** |

---

## 5. `v7.3` Active 5G Thermal Optimization (`~36–37 °C` Target, Zero Max-Clock Capping)

Measured on pure cellular WWAN (`wifi_enabled=0`, `rmnet_data1` + `rmnet_data2`, zero `rndis0`/`wlan2`):

| Metric | BEFORE (`0x028708` 3G-only FD, `com.heytap.mcs` TCP retransmits, volatile `OEM_DENY_3`) | AFTER (`0x1AA708` 5G FD `5s/3s`, 31-domain `hosts` sinkhole, `cos17_thermal_once` v8, `rmnet_ipa0=3c`) | Delta |
| :--- | :---: | :---: | :---: |
| **Active `xt_idletimer` 4G/5G FD Timers** | `0` (only `100`) | **`FD_rmnet_data1` + `FD_rmnet_data2` active** | **4G/5G FD ON** |
| **60s Active 5G Modem Die (`mdmss-0`)** | `47.3 -> 44.2 °C` (`sdr0` climbed `41 -> 42 °C`) | **`49.2 -> 44.9 °C` (`-4.3 °C` rapid drop)** | **Fast cooldown** |
| **180s Light Active 5G (HTTPS + 120 Hz Scroll) — Battery** | `39.0 – 40.0 °C` (stalled, unable to cool) | **Cooled `38.3 -> 37.2 °C` & held `37.2 °C` (`+0.0 °C` rise)** | **Flat `+0.0 °C`** |
| **5G RF Power Amp (`pa_therm0`)** | Peaked up to **`58.7 °C`** (`mcs` `Send-Q=1243`, `3 pkts/s`) | **`40.8 – 41.5 °C`** | **`-17.2 °C`** |
| **5G RF Transceiver (`sdr0`) on Standby** | `41.0 – 42.0 °C` (stuck `RRC_CONNECTED`) | **`-273.0 °C` (hardware power collapse)** | **True RF idle** |
| **CPU / GPU Max Clocks** | `3.53 / 4.32 GHz`, `1,100 MHz` | **`3.53 / 4.32 GHz`, `1,100 MHz`** | **0% capped** |

---

## 6. `v7.3` Sub-48 °C Scrolling, Smart Sidebar `"All"` & SystemUI Smoothness (`4 ms / 960 MHz / ed=0` WALT)

Measured under unplugged-equivalent power (`mca_charge_interface` `input_suspend` eliminating `+2.2 W` USB charging floor) with **zero CPU/GPU max-clock capping**:

| Workload / Metric | BEFORE (`v7.2` / `SCHEDBOOST=2` / Uncompiled) | AFTER (`v7.3` `4ms/960MHz/ed=0` Sweet Spot + `dexopt_ver=75`) | Delta / Verdict |
| :--- | :---: | :---: | :---: |
| **`com.android.settings` (6× Simple 120 Hz Scroll)** | `p0=2.40 GHz`, `max_cpu=59.4–63.7 °C`, `run-from-apk` | **`1,722` frames, `0.23%` jank (`0.41%` legacy), `p50=8 / p90=10 / p95=11 / p99=12 ms`, `Missed Vsync=0`, `Slow UI=0`**, `p0=1.15–1.36 GHz`, `p6=1.02 GHz`, `sb=0`, `gpu=222 MHz`, **`max_cpu=47.8–49.8 °C`** (`+9.8 °C` Δ over batt, `+0.0–0.1 °C` batt rise) | **`-13.9 °C` CPU die, locked 120 Hz** |
| **`com.coloros.smartsidebar:ui` (5× Edge Swipe -> `"All"`)** | `verify`, `max_cpu=63.3 °C`, `p0=1.78–2.40 GHz` | **`speed` AOT**, `1,042` frames, **`1.63%` legacy jank**, **`p50=9 / p90=15 / p95=16 / p99=22 ms`**, `Missed Vsync=0`, `sf_idle=0`, `p0=748–960 MHz`, **`max_cpu=47.8–49.8 °C`** | **`-13.5 °C` CPU die, smooth `"All"`** |
| **`com.android.systemui` (Interactive QS -> Notif -> Expand -> Dismiss)** | `run-from-apk`, `4.98–9.30%` jank, `p99=61–81 ms`, `Slow UI=166–312`, `id=3` post-Doze trap | **`speed` AOT (`101 MB` `.odex`)**, **`5/5` `id=1@120.00001Hz`**, `820` frames, **`1.95%` jank**, **`p50=10 / p90=14 / p95=16 / p99=46 ms`**, `Slow UI=15`, `Missed Vsync=3`, `Davey=0`, `Skipped=0`, `Unknown RenderIntent=0` | **`-61%` jank, `-90%` Slow UI** |
| **`com.android.launcher` (5-Cycle Unlock -> Settings)** | `verify`, `p99=22 ms`, `5.81%` legacy | **`speed-profile`**, **`1.97%` jank (`4.37%` legacy)**, `p50=7 / p90=11 / p95=15 / p99=21 ms` | **PASS** |
| **Unplugged Screen-Off Idle / Cooldown (Dual-SIM LTE + AOD)** | USB `+2.2 W` heat mask | **`63 – 72 mA`** (`~0.25 W`), `CPU busy = 1.07–1.14%`, `gpu = 222 MHz`, `quiet_therm` **`-0.9 °C/min`** | **Fast cooldown** |

---

## 7. `v7.3` Stability, Soft-Reboot Immunity, Fork-Stall & Deep-Doze FCM Proof

| Verification Gate | BEFORE (`v7.2`) | AFTER (`v7.3`) | Verdict |
| :--- | :---: | :---: | :---: |
| **Doze / 60 Hz / 120 Hz + 10× Fullscreen AOD Torture** | Deterministic `RefreshRateSelector` `SIGABRT` soft reboot | **`0` restarts** across `10/10` Fullscreen AOD (`15 s` Doze) + `5/5` combined torture (`0` tombstones) | **100% Immune** |
| **Consecutive Reboot Stress (`5×` Material You + `3×` Final Gate)** | Intermittent post-unlock stalls / `StrictJarManifest` NPEs | **`8/8` boots (`30.6–32.8 s`)**, `0` PID restarts, `0` tombstones, `0` AVCs, `0` `StrictJarManifest` NPEs | **100% PASS** |
| **Post-Unlock Zygote `Slow operation` Fork Stalls** | **`122` stalls (`2,289–2,643 ms` each)** | **`0` stalls** (`libnyako_core.so` 17-scope fast-path + `cos17_thermal_once` v8) | **`-100%`** |
| **`libnyako_core.so` Static RX Trampoline Pool Headroom** | Unverified | **`g_pool_off = 0 / 262,144 B` (`64/64` pages free)** in `system_server` & `SystemUI` | **100% free** |
| **Rootless Deep-Doze FCM (`dumpsys deviceidle force-idle deep`)** | Blocked by CN GMS (`cn.google.services`) / Hans / Proxy | **`mtalk.google.com:5228` `ESTAB` (`4–6 ms` heartbeat)**; GMS `c2dm.intent.RECEIVE` flag rewritten `0x400010 -> 0x30` in **`+3 ms`**; force-stopped receiver cold-started in **`+18 ms`** | **Zero-delay** |

---

## 8. Final `v7.3.1` Pure Non-Root (`uid=2000`) Clean-Flash Verification

Verified after a full clean flash (`fastboot flash super` + `erase userdata` + `erase metadata`, **pure non-root `uid=2000` `u:r:shell:s0`, zero Magisk/KernelSU/LSPosed/`service.sh`**):

| Verification Check | `v7.3` Clean-Flash (Before `v7.3.1`) | `v7.3.1` Clean-Flash (After) | Verdict |
| :--- | :---: | :---: | :---: |
| **`QtiImsExtService` / `QtiImsExtConnector` Bind Storm** | **`11,734`** hits in 34 min (`~6–10/s` forever) | **`0` / `0`** (`ims-ext-common.jar` `resolveService` guard, `0` stale `oat/fsv_meta`, `0` DEX/Verify errors) | **FIXED (`-100%`)** |
| **Settled Screen-OFF `Asleep` Total CPU Busy (`/proc/stat`)** | `3.1 – 3.3%` (`Dozing`) | **`0.79%`** (`99.21%` idle — **`-75%` idle CPU!**) | **PASS (`< 1.50%`)** |
| **`cos17_daemons.rc` `swappiness` & `kswapd0` 20s Delta** | `swappiness 100` (`1.74M` boot swap-outs, `kswapd0` `56 s`) | **`swappiness 60`**, `kswapd` 20s delta **`0` ticks** (`pgscan_kswapd=0`, `pswpout=0`) | **FIXED** |
| **`com.heytap.market` (App Market) Policy** | Unrestricted | `com.heytap.market:allow`, `WAKE_LOCK/RUN_IN_BACKGROUND/RUN_ANY_IN_BACKGROUND: allow` | **UNTOUCHED (PASS)** |
| **Rootless WALT / uclamp / `sconfig` Parity** | Native | `p0` `4000/0/960000/92/748800/-15`, `p6` `4000/1209600/1017600`, `display.uclamp.min=2.00`, `sconfig=0`, `cpuinfo_max=3.53/4.32 GHz` | **100% Native PASS** |
| **Rootless One-Shot Services & ART Dexopt** | Native | `dexopt_ver=75` (`SystemUI=speed`, `SmartSideBar=speed`, `Launcher=speed-profile`), `hygiene_ver=75`, `13/13` installed CN telemetry pkgs `OEM_DENY_3:deny` | **PASS** |
| **Telephony / VoLTE / 5G NR EN-DC / Dual-SIM** | Active | `imsdaemon` & `qcrilNrd` alive; Dual-SIM LTE + 5G NR EN-DC (`isNrAvailable=true, isEnDcAvailable=true, mVopsSupport=2/3`), Fast Dormancy `0x1AA708` | **PASS** |
| **Rootless LTPO, FCM & Security Hygiene** | Native | `SELinux Enforcing`, `secilc -N NEW_PAIRS=0`, `qemu=0`, `oplusex=0`, `overlay=v4-hw-ltpo-ready` (`sf_idle=1`), `cn.google.services=ABSENT`, `4/4` GMS `deviceidle` whitelists, live `libNyako_hook_fcmsys.so` callbacks (`0` LSPosed), `Real_FATAL=0`, `Real_StrictJarManifest=0`, `0` PID restarts | **100% PASS** |
| **Companion GKI Kernel (`wild-trsy r24`)** | `6.6.118-android15-wild-trsy` | `workqueue/power_efficient=Y`, `rcutree/enable_rcu_lazy=Y`, `rcu_nocbs=0-7`, `ABI gate: 9,317 symbols, 0 differ`, clean idle battery **`31.4 °C`** | **100% KMI-safe** |
