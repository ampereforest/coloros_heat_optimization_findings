# ColorOS 17 Thermal, Stability, Smoothness & Zero-Delay FCM Optimization Findings (`dada` / SM8750)

Reproducible root-cause analysis, verified unified `.patch` files, zero-polling C/C++/Java/smali sources, offset-verified AArch64 ELF patchers, and KMI-safe GKI kernel configs for eliminating thermal buildup, UI scroll/unlock stutter, `SurfaceFlinger` soft reboots, standby drain, and CN FCM push delays on **ColorOS 17** ported to **Xiaomi 15 (`dada`, Snapdragon 8 Elite / SM8750)**.

---

## Exact Target Base Port

| Attribute | Value |
| :--- | :--- |
| **Target Device** | Xiaomi 15 (`dada`, Qualcomm SM8750 / `sun`, 12 GB RAM, BOLED 1–120 Hz LTPO) |
| **Ported OS Base** | **ColorOS 17 V6 CN — `PLZ110_17.0.0.107(CN01)`** (OnePlus 15T base, Android 17 / API 37) |
| **Vendor / ODM Hardware Base** | Xiaomi 15 Stock HyperOS `OS4.0.0.11.XOCMIXM` (`vendor` / `odm` HALs & firmware) |
| **Optimization Build Milestone** | **`v7.3.1`** (100% rootless-baked `super.img`, verified live in pure non-root `uid=2000` `SELinux Enforcing` with zero `service.sh` / zero Magisk / zero KernelSU / zero LSPosed dependency) |
| **Companion GKI Kernel** | **`6.6.118-android15-wild-trsy` (`r24`)** ([`ampereforest/GKI_KernelSU_SUSFS`](https://github.com/ampereforest/GKI_KernelSU_SUSFS/releases/tag/r24)) |

> **Important for Porters:** All unified `.patch` files under [`patches/`](patches/) and AArch64 binary patchers under [`scripts/`](scripts/) are pinned against the exact base port above and verified via `patch --dry-run` + clean apply. Every ELF patcher verifies ELF headers, original MD5 hashes, and exact instruction bytes before modifying a single byte.

---

## Architectural Principles

Every optimization in this repository obeys five strict engineering rules:

1. **Zero Clock-Capping / Zero Performance Loss:** Never lower `scaling_max_freq` on CPU clusters (`3.53 GHz` Oryon-M / `4.32 GHz` Oryon-L Prime) or Adreno GPU (`1,100 MHz`). Heat is eliminated by removing *wasted work* (polling loops, `QtiImsExtConnector` bind storms, `SCHEDBOOST=2` scroll spikes, 5G TCP retransmit locks, SELinux AVC storms, runaway thermal overrides, and `kswapd0` thrashing), never by handicapping peak performance.
2. **100% Rootless Architecture:** Every init trigger, property override, SELinux CIL rule, Nyako native hook (`overlay.so`, `libNyako_hook_ltpofix.so`, `libNyako_hook_fcmsys.so`, `libNyako_hook_fcmbatt.so`), smali guard (`ims-ext-common.jar`), and pure-C one-shot helper bakes directly into `super.img` with **zero root / zero Magisk / zero KernelSU / zero LSPosed runtime dependency**.
3. **Zero Polling Loops:** Every custom daemon (`hdrpeakd`, `overlay.so`, `ltpofix_plugin.cpp`, `xmvibproxy_v2`, `cos17_thermal_once`, `cos17_dexopt_once`, `cos17_memkiller_ctl`) is either strictly **one-shot** or **kernel futex / AIDL / `CLOCK_MONOTONIC` condvar event-driven** (`0.000%` idle CPU).
4. **Zero Destructive Debloat:** No user-facing ColorOS apps or features are deleted. Background CN telemetry is contained non-destructively via Android 15 kernel eBPF (`FIREWALL_CHAIN_OEM_DENY_3`), per-package/UID `appops`, and `/system/etc/hosts` sinkholing, while keeping `com.heytap.market` (App Market, 100% unrestricted), `com.heytap.mydevices` (Bluetooth UI), Gallery Editor + AI (`com.oplus.aiunit` / `com.oplus.deepthinker`), OpenEUICC eSIM, OShare, Titanium Browser, 1–120 Hz LTPO, 3200-nit HDR, and O-Haptics fully intact.
5. **Strict Enforcing SELinux Hygiene:** Zero DirtySepolicy rules, zero `*qemu*` properties, `0` new SELinux AVCs at boot/runtime, 16 KB ELF `PT_LOAD` alignment across all compiled binaries/libraries, and `secilc -N` **`NEW_PAIRS=0`**.

---

## Key Benchmark Summary (`BEFORE` vs. `AFTER` on Bare Device, Zero External Fan)

| Test Scenario / Metric | Before (Base ColorOS 17 V6 / Pre-`v7.3.1`) | After (`v7.3.1` Rootless + `wild-trsy r24`) | Delta |
| :--- | :---: | :---: | :---: |
| **Settled Screen-OFF `Asleep` CPU Busy (Pure Non-Root)** | `~10–15%` base / `3.1–3.3%` (`v7.3`) | **`0.79%`** (`99.21%` idle!) | **`-75%` vs `v7.3`** |
| **Unplugged Dual-SIM Idle Current (LTE + AOD Dozing)** | `~350–850 mA` (`3.3–3.9 W`) | **`63 – 72 mA`** (`~0.25 W`) | **`-93%`** |
| **`QtiImsExtConnector` Failed Bind Storm (`34 min`)** | `11,734` hits (`~6–10` Binder IPCs/s) | **`0` hits** (`imsdaemon` 5G/VoLTE intact) | **`-100%`** |
| **Light Active 5G (180s HTTPS + 120 Hz Scroll) — Battery** | `39.0 – 40.0 °C` (`pa_therm0 58.7 °C`) | **`37.2 °C`** (`+0.0 °C` rise, `pa 40.8–41.5 °C`) | **`-17.2 °C` PA** |
| **Simple 120 Hz Scrolling — CPU Die / Jank / `p99`** | `63.7 °C` (`policy0=2.4 GHz`, `sb=2`) | **`47.8 °C`** (`0.23%` jank, **`p99 = 12 ms`**, `sb=0`) | **`-13.9 °C` CPU** |
| **Smart Sidebar -> `"All"` — CPU Die / Jank / `p99`** | `63.3 °C` (`verify`, `p0=1.78–2.4 GHz`) | **`47.8–49.8 °C`** (`speed` AOT, `1.63%` legacy, `p99=22 ms`) | **`-13.5 °C` CPU** |
| **Interactive SystemUI (QS -> Notif -> Expand -> Dismiss)** | `4.98–9.30%` jank (`p99=61–81 ms`, `run-from-apk`) | **`1.95%` jank** (`p99=46 ms`, `101 MB` `speed` `.odex`, `5/5` `120 Hz`) | **`-61%` jank** |
| **Post-Unlock Zygote `Slow operation` Fork Stalls** | `122` stalls (`2.3–2.6 s` each) | **`0` stalls** (`nyako_avc = 0`, `64/64` RX pool free) | **`-100%`** |
| **`SurfaceFlinger` Doze / AOD / 60 Hz `SIGABRT` Soft Reboots** | Deterministic crash on Doze/60 Hz | **`0` restarts** (`10/10` AOD + `5/5` torture + `3/3` reboots) | **`-100%`** |
| **Rootless Deep-Doze FCM Push Wake (Force-Stopped App)** | Blocked / delayed by CN GMS & Hans | **`+3 ms` flag rewrite / `+18 ms` cold wake** (`:5228 ESTAB`) | **Zero-delay** |
| **`kswapd0` Reclaim Thrashing (`swappiness=60`, `wsf=20`)** | `20.72 s` / `1.74M` boot swap-outs (`swappiness=100`) | **`0` ticks settled** (`pgscan=0`, `pswpout=0`) | **`-100%`** |
| **240s 120 Hz Light Usage — `CPU0` `NET_RX` Softirqs** | `8,102` (`rmnet=3d`, `rmnet_ipa0=02/00`) | **`798`** (`rps_cpus=3c` on `rmnet_ipa0`/`rmnet_data`/`rndis0`/`wlan*`) | **`-90.2%`** |
| **`oplus-color-mode-sync` (70-min settled)** | `8,854` jiffies (`2 Hz` fork) | **`0` jiffies** (kernel futex sleep) | **`-100%`** |
| **`libcameraopt.so` SELinux AVCs (30 min)** | `21,163+` denials | **`0` denials** (`0` `audit_lost`) | **`-100%`** |
| **HDR Peak (`hdrpeakd`) & Hardware 1–120 Hz LTPO Idle CPU** | `10.8%` / `~4–6%` (`memfd` poll) | **`0.000%`** (`hdrpeakd`) / **`0.06%`** (`overlay.so` v4.1) | **`-98–100%`** |
| **4K60 H.264 Browser Video — SF CPU / Power / CWB** | `251j` / `3,819 mW` / `39` CWB (`200 ms`) | **`44j` (`-82.5%`) / `2,618 mW` (`-31.4%`) / `8` CWB (`1000 ms`)** | **`-1,201 mW`** |
| **GKI Kernel KMI/ABI Drift (`android15-6.6`)** | Stock baseline (`9,317` symbols) | **`0` symbols differ** (`wq_power_efficient=Y`, `rcu_lazy=Y`) | **100% KMI-safe** |

Full raw tables and measurement methodology are documented in [`docs/BENCHMARKS-AND-TELEMETRY.md`](docs/BENCHMARKS-AND-TELEMETRY.md).

---

## Repository Layout

```text
.
├── README.md
├── docs/
│   ├── ROOT-CAUSES.md                         # Forensic analysis of all 10 categories + Thermal Boomerang pitfalls
│   ├── BENCHMARKS-AND-TELEMETRY.md            # Complete BEFORE vs. AFTER telemetry (Pass 1–3 & v7.1 -> v7.3.1)
│   └── KERNEL-GKI-TWEAKS.md                   # android15-6.6 KMI-safe power tweaks & runtime verification guide
├── patches/
│   ├── 01-thermal-perf/                       # PerfBreak neutralization, v7.3.1 perfboostsconfig (0 scroll SCHEDBOOST=2), perfconfigstore, sys_thermal_config, thermallevel_to_fps
│   ├── 02-display-ltpo-cwb/                   # CWB 1000ms throttle, overlay.so v4.1 + LtpoFixHooks v8.1 deployment guide
│   ├── 03-telephony-wifi-rps/                 # 4G/5G Fast Dormancy (0x1AA708), 31-domain CN hosts sinkhole, netd.rc + plat_file_contexts (oplus_dnsproxyd), init.oplus.debug.rc, my_product/cust radio props, WCNSS_qcom_cfg, midasd/ostatsd/init.rc, SELinux CIL
│   ├── 04-memory-vm-props/                    # post_boot WatermarkScale=20, system & vendor build.prop hygiene
│   ├── 05-kernel-gki-defconfig/               # thermal-efficiency.fragment, GitHub Action, build.yml hook patch
│   └── 06-stability-fcm-ims/                  # v7.3.1 QtiImsExtConnector.smali patch, dual cn.google.services removals, DeviceIdle GMS whitelist, deployment notes
├── src/
│   ├── daemons/                               # cos17_thermal_once.c (v8), cos17_dexopt_once.c (v2), cos17_memkiller_ctl.c, hdrpeakd.cpp, build_ndk.sh (16KB-aligned)
│   ├── fcm/                                   # fcm_plugin.cpp, java/com/nyako/fcm/FcmHooks.java, build_fcm.sh (split-scope fcmsys + fcmbatt)
│   ├── haptics/                               # xmvibproxy_v2.cpp (Setup Wizard code=6 stop & single-envelope RTP)
│   ├── init/                                  # init.cos17_dada_perf.rc (4ms/960MHz/ed=0 WALT + 6.6 input_boost + rmnet_ipa0 RPS), cos17_daemons.rc (swappiness 60), cos17_dexopt.rc, hdrpeak.rc, ltpo_overlay.rc
│   ├── ltpo/                                  # overlay.cpp (v4.1 RefreshRateSelector crash guard), ltpofix_plugin.cpp (v8), Java LtpoFixHooks (v8.1)/PanelSafety/PointerMonitor
│   ├── nyako/                                 # nyako_scope_filter.c (17-scope freestanding PC-relative matcher)
│   └── ui/                                    # VideoBlurSuppressionController.java (R14 smart video blur suppression)
└── scripts/
    ├── patch_color_mode_sync.py               # Offset-verified ELF patcher: eliminate 2 Hz settings fork storm
    ├── patch_libnyako_scope.py                # Offset-verified ELF patcher: libnyako_core.so 17-scope fast-path + UID-0 chmod + fsetxattr=0
    ├── patch_libopex.py                       # Offset-verified ELF patcher: libopex.so static opex bypass
    ├── patch_pass2_culprits.py                # Offset-verified ELF patcher: libcameraopt, citsensorservice, libatlasservice, horae
    ├── patch_sensors_qsh_cct.py               # Offset-verified ELF patcher: sensors.qsh.so 0x1fa2672 -> 0x1fa2692
    └── patch_surfaceflinger_renderintent.py   # Offset-verified ELF patcher: SurfaceFlinger RenderIntent translation
```

---

## How Porters Apply These Fixes

### 1. Apply Unified Config, XML, Init & Property Patches (`patches/`)
From the root of your unpacked ROM tree (containing `system/`, `system_ext/`, `product/`, `my_product/`, `my_region/`, `my_stock/`, `vendor/`, `odm/`):

```bash
# 01 — Thermal HAL & Perf Moderation (v7.3.1: zero scroll SCHEDBOOST=2, zero GPU 680 MHz floor, zero max-clock caps)
patch -p1 < patches/01-thermal-perf/vendor_thermalbreakboostconfig.patch
cp patches/01-thermal-perf/odm_thermalbreakboostconfig.xml odm/etc/thermalbreakboostconfig.xml
patch -p1 < patches/01-thermal-perf/vendor_perfboostsconfig.patch
patch -p1 < patches/01-thermal-perf/vendor_perfconfigstore.patch
patch -p1 < patches/01-thermal-perf/odm_sys_thermal_config.patch
patch -p1 < patches/01-thermal-perf/my_stock_sys_thermal_config.patch
patch -p1 < patches/01-thermal-perf/vendor_thermallevel_to_fps.patch

# 02 — Display, LTPO & CWB
patch -p1 < patches/02-display-ltpo-cwb/vendor_cwb_display_props.patch

# 03 — Active 5G Fast Dormancy, CN Sinkhole, Wi-Fi Offload, Socket Labels & Init Hygiene
patch -p1 < patches/03-telephony-wifi-rps/vendor_dpm_fast_dormancy.patch
patch -p1 < patches/03-telephony-wifi-rps/system_ext_dpm_fast_dormancy.patch
patch -p1 < patches/03-telephony-wifi-rps/system_hosts_cn_telemetry_sinkhole.patch
patch -p1 < patches/03-telephony-wifi-rps/vendor_wcnss_qcom_cfg.patch
patch -p1 < patches/03-telephony-wifi-rps/system_init_rc_hygiene.patch
patch -p1 < patches/03-telephony-wifi-rps/system_netd_oplus_dnsproxyd.patch
patch -p1 < patches/03-telephony-wifi-rps/system_plat_file_contexts_dnsproxyd.patch
patch -p1 < patches/03-telephony-wifi-rps/system_ext_init_oplus_debug_rc.patch
patch -p1 < patches/03-telephony-wifi-rps/system_ext_midasd_rc.patch
patch -p1 < patches/03-telephony-wifi-rps/system_ext_ostatsd_rc.patch
patch -p1 < patches/03-telephony-wifi-rps/my_product_ims_audio_props.patch
patch -p1 < patches/03-telephony-wifi-rps/my_product_cust_cn_regionlock.patch

# 04 — Memory VM & Rootless Build Properties
patch -p1 < patches/04-memory-vm-props/vendor_post_boot_memory_watermark.patch
patch -p1 < patches/04-memory-vm-props/system_build_prop_hygiene.patch
# Note: vendor_build_prop_thermal_hygiene.patch and vendor_cwb_display_props.patch
# both append to vendor/build.prop; merge the appended blocks if applying both.

# 06 — Rootless Zero-Delay FCM & QtiImsExtConnector Bind-Storm Elimination (v7.3.1)
patch -p1 < patches/06-stability-fcm-ims/my_product_cn_gms_features.patch
patch -p1 < patches/06-stability-fcm-ims/odm_gnss_bds_preference.patch
patch -p1 < patches/06-stability-fcm-ims/my_region_deviceidle_whitelist.patch
```

For `product/framework/ims-ext-common.jar` (`patches/06-stability-fcm-ims/product_ims_ext_common_qti_connector.smali.patch`), disassemble via `baksmali`, apply the smali patch, reassemble with `smali a -a 28`, repack 4-byte aligned `ZIP_STORED`, and **remove the 6 stale `product/framework/oat/{arm,arm64}/ims-ext-common.{odex,vdex,fsv_meta}` files** (see [`patches/06-stability-fcm-ims/stability_fcm_ims_notes.md`](patches/06-stability-fcm-ims/stability_fcm_ims_notes.md)).

Merge [`patches/03-telephony-wifi-rps/system_sepolicy_cos17_snippet.cil`](patches/03-telephony-wifi-rps/system_sepolicy_cos17_snippet.cil) into both `/system/etc/selinux/plat_sepolicy.cil` and `/system_ext/etc/selinux/plat_sepolicy_debug.cil`, validate with `secilc -N` (`NEW_PAIRS=0`), and update `/system/etc/selinux/plat_sepolicy_and_mapping.sha256`.

### 2. Compile Native 16 KB-Aligned Zero-Polling Helpers, FCM Plugins & Install Init `.rc` Files (`src/`)
```bash
# Core daemons + overlay.so v4.1 (pass STATSLOG_BOOTSTRAP_SO if replacing PLZ110 /system/lib64/overlay.so)
ANDROID_NDK_HOME=/path/to/android-ndk-r29 bash src/daemons/build_ndk.sh ./out/arm64-v8a

# Split-scope rootless FCM plugins (libNyako_hook_fcmsys.so + libNyako_hook_fcmbatt.so)
ANDROID_NDK_HOME=/path/to/android-ndk-r29 \
ANDROID_BUILD_TOOLS=/path/to/android-sdk/build-tools/37.0.0 \
LIBNYAKO_CORE_SO=/path/to/libnyako_core.so \
bash src/fcm/build_fcm.sh ./out/fcm
```
Install compiled binaries and `src/init/*.rc`:
- `cos17_thermal_once` (v8), `cos17_dexopt_once` (v2), `cos17_memkiller_ctl`, `hdrpeakd`, `xmvibproxy` -> `/vendor/bin/` (`0755`, matching `vendor_file_contexts` labels: `cos17_memkiller_exec` / `hdrpeak_exec`)
- `overlay.so` (v4.1), `libNyako_hook_ltpofix.so` (v8.1), `libNyako_hook_fcmsys.so`, `libNyako_hook_fcmbatt.so` -> `/system/lib64/` (`0644`, `system_file`)
- `src/init/{init.cos17_dada_perf,cos17_daemons,cos17_dexopt,hdrpeak}.rc` -> `/vendor/etc/init/` (`0644`)
- `src/init/ltpo_overlay.rc` -> `/system/etc/init/` (`0644`)

### 3. Run Offset-Verified AArch64 Binary Patchers (`scripts/`)
Every script checks ELF headers and exact original instructions before patching (use `--verify-only` first to dry-run):

```bash
python3 scripts/patch_surfaceflinger_renderintent.py -i system/bin/surfaceflinger -o out/surfaceflinger
python3 scripts/patch_color_mode_sync.py             -i odm/bin/hw/oplus-color-mode-sync -o out/oplus-color-mode-sync
python3 scripts/patch_sensors_qsh_cct.py             -i vendor/lib64/sensors.qsh.so -o out/sensors.qsh.so
python3 scripts/patch_libopex.py                     -i system_ext/lib64/libopex.so -o out/libopex.so
ANDROID_NDK_HOME=/path/to/android-ndk-r29 \
python3 scripts/patch_libnyako_scope.py              -i system/lib64/libnyako_core.so -o out/libnyako_core.so
python3 scripts/patch_pass2_culprits.py \
  --libcameraopt    vendor/lib64/libcameraopt.so \
  --citsensor       odm/bin/hw/vendor.xiaomi.sensor.citsensorservice.aidl \
  --libatlasservice system_ext/lib64/libatlasservice.so \
  --horae           system_ext/bin/horae \
  --out-dir         out/pass2
```

### 4. Companion GKI Kernel Config (`patches/05-kernel-gki-defconfig/`)
See [`docs/KERNEL-GKI-TWEAKS.md`](docs/KERNEL-GKI-TWEAKS.md) for adding `CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y` and `# CONFIG_RCU_LAZY_DEFAULT_OFF is not set` to any `android15-6.6` GKI kernel build with zero KMI/ABI drift.

---

## Critical Pitfalls & Thermal Boomerang Lessons Learned (Read Before Porting!)

1. **NEVER stop `vendor.diag-router` on SM8750 (`sun`):** Qualcomm `libdiag.so` enters a **100% CPU `error:111` busy-loop** if the `diag-router` socket closes.
2. **NEVER set `persist.vendor.debug.cwb.disable=true` on Xiaomi 15 (`dada`):** Disabling Client WriteBack breaks Xiaomi 15's under-display BOLED TCS3720 ALS/Auto-Brightness & Proximity readings. Throttle CWB via `ro.vendor.cwb.timeout=1000` (`-79.5%` captures, 100% ALS accuracy) and include the `cwb.disable=false` dirty-flash reset in `init.cos17_dada_perf.rc`.
3. **NEVER drop raw Stock `OS4.0.0.11` `perfboostsconfig.xml` into ColorOS 17:** Raw Stock re-introduces a **`5000 ms` `680 MHz` GPU floor** on `0x00001097` (`Large Comp Hint`) and `2745/4089` boost floors/caps, causing an immediate **`+0.9 °C` thermal boomerang in 75 seconds**. Always use `patches/01-thermal-perf/vendor_perfboostsconfig.patch` (zero `SCHEDBOOST=2` on scroll hints, zero GPU floor, zero CPU/GPU max-clock caps).
4. **NEVER set `up_rate_limit_us=10000` (`10 ms`) on a `120 Hz` (`8.33 ms` frame) display:** Waiting `10 ms` to scale up exceeds a full `120 Hz` Vsync period and regresses Smart Sidebar `"All"` jank from `2.52% -> 11.32%`. Use the **`4000 µs` (`4 ms`) / `960 MHz` `input_boost` (`60 ms`) / `sched_ed_boost=0`** sweet spot (verified `0.23%` scroll jank at `47.8 °C`).
5. **NEVER block UID-1000 packages via `cmd connectivity set-package-networking-enabled false` (`OEM_DENY_3`):** `OEM_DENY_3` keys on **UID**, not package name — blocking UID `1000` (`com.heytap.mcs`, `com.oplus.obrain`, `com.oplus.midas`, `com.oplus.onetrace`) blocks `system_server` networking system-wide! Restrict UID-1000 packages via per-package `appops` (NEVER `--uid`) and `/system/etc/hosts` sinkholing; also remember `OEM_DENY_3` is **volatile in kernel BPF RAM** and resets on reboot unless re-applied every boot (`cos17_thermal_once` v8).
6. **NEVER forget `cos17_daemons.rc` `disable_mem_killer=0` -> `swappiness` conflict:** Leaving `write /proc/sys/vm/swappiness 100` in the `disable_mem_killer=0` trigger silently overwrites `swappiness=60` on clean flash and caused `1.74M` boot-time swap-outs (`kswapd0` `56 s` CPU) with `6.8 GB` RAM free.
7. **NEVER put `system_prop` / `vendor_ims_prop` or `proc_watermark_scale_factor` writes in `vendor_init`:** Under Enforcing SELinux, `vendor_init` is denied `system_prop` / `vendor_ims_prop` writes. Place system/radio triggers in `/system/etc/init/hw/init.rc` or `/system_ext/etc/init/init.oplus.debug.rc` (`u:r:init:s0`) and set `WatermarkScale=20` inside `/vendor/bin/init.kernel.post_boot-memory.sh` + `cos17_thermal_once`.

---

## License

All custom C/C++/Java sources, Python patch scripts, init files, and documentation in this repository are licensed under the [MIT License](LICENSE). No proprietary vendor or OEM binaries are distributed in this repository.
