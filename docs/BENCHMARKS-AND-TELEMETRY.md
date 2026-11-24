# Benchmarks & Live Telemetry Ledger (`BENCHMARKS-AND-TELEMETRY.md`)

All measurements below were captured on a **bare Xiaomi 15 (`dada`, SM8750, 12 GB RAM)** with **zero external cooling fan**, running **ColorOS 17 V6 CN (`PLZ110_17.0.0.107(CN01)`)** as fixes were progressively engineered from Pass 1 through the final rootless **`v7.2`** clean flash + **`6.6.118-android15-wild-trsy` (`r24`)** kernel.

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
| **Hardware 1–120 Hz LTPO (`overlay.so` + `pipe2`)** | `~4–6%` SF CPU (`10 Hz` `publishUni` + `1 MB` `memfd` poll) | **`0.06%` SF CPU at 1 Hz** (`fps1_times` active) | **`-98%`** |
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
| **5G RF Transceiver (`sdr0`, eBPF `OEM_DENY_3`)** | `42.0 – 43.0 °C` (stuck `RRC_CONNECTED` via Baidu TCP retransmits) | **`-273.0 °C` (RF OFF)** with 5G/LTE & Wi-Fi enabled | **True RF idle** |

### Part B — 240 s Real-World 120 Hz Light Usage (120 s Homescreen L/R Scroll + 120 s Settings U/D Scroll)

| Metric | BEFORE | AFTER | Delta |
| :--- | :---: | :---: | :---: |
| **Battery Temp (`t = 240 s`)** | `40.3 °C` | **`38.2 °C`** | **`-2.1 °C`** |
| **Settings Scroll — Avg CPU Zones ≥ 50 °C (`GT50`)** | `13.1` zones/sample (`54.4 °C` peak) | **`2.25` zones/sample** | **`-83%`** |
| **`kswapd0` Cumulative CPU Time** | `20.72 s` | **`0.00 s`** | **`-100%`** |
| **`CPU0` `NET_RX` Softirqs** | `8,102` | **`798`** | **`-90.2%`** |
| **Medium Cluster (`policy0`) Residency at Min `556.8 MHz`** | `141,390 ms` (`53.5%`) | **`154,980 ms` (`58.7%`)** (`86.2% <= 1.15 GHz`) | **`+13.6 s` min-freq** |

---

## 5. Final `v7.2` Rootless Clean-Flash + `wild-trsy` `r24` Runtime Verification

Verified after a complete clean flash (`fastboot flash super` + `erase userdata` + `erase metadata`, **zero `service.sh`**) with companion GKI kernel `6.6.118-android15-wild-trsy` (`r24`):

| Verification Check | Observed Result |
| :--- | :--- |
| **Settled Screen-OFF Total CPU Busy** | **`1.78%` busy / `98.22%` idle** (`policy0` `94.5% <= 556.8 MHz`, `policy6` `100.0% @ 1017.6 MHz`) |
| **Settled Idle Battery Temp (with `wild-trsy r24`)** | **`31.4 °C`** (`+0.0 °C` delta during Settings scroll test) |
| **Rootless Gap A (System-Domain `init.rc`)** | `persist.sys.midas_ostats.restriction=1`, `persist.oplus.qspa.modem=disabled`, `wifi.fulldump=0`, `ims.loglevel=0`, **`0` SELinux AVCs** |
| **Rootless Gap B (`wifi_service` CIL Rule)** | `cmd wifi get-poll-rssi-interval-msecs = 6000` (`6000 / 10000 ms`), `secilc -N` **`NEW_PAIRS=0`**, **`0` AVCs** |
| **Rootless Gap C (VM & RPS)** | `vm.swappiness = 60`, `vm.watermark_scale_factor = 20`, `rps_cpus = 3c` (`wlan0`, `wlan2`, `rndis0`, `rmnet_data0..3`) |
| **Network Sanity (Zero Degradation)** | `ping 1.1.1.1`: `5/5`, **`50.45 ms`** avg (vs `60.5 ms` before); `5 MB` HTTP download: **`7.41 MB/s` (`59.3 Mbps`, HTTP 200)** |
| **Companion GKI Kernel Runtime (`wild-trsy r24`)** | `workqueue/power_efficient = Y`, `rcutree/enable_rcu_lazy = Y`, `/proc/cmdline` `rcu_nocbs=0-7`, `ABI gate: OK (9,317 symbols, 0 differ)` |
| **Core Features Gate** | 1–120 Hz LTPO (`v3-hw-ltpo-ready`), 3200-nit HDR (`hdrpeakd 0.0%`), CWB Auto-Brightness, Dual-SIM 5G NSA + VoLTE, Gallery AI & App Market on-demand (`0.0%` idle), `xmvibproxy 0.0%` — **100% PASS** |
