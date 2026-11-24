# ColorOS 17 Thermal & Idle Power Optimization Findings (`dada` / SM8750)

Reproducible root-cause analysis, verified unified `.patch` files, zero-polling C/C++/Java sources, offset-verified AArch64 ELF patchers, and KMI-safe GKI kernel configs for eliminating thermal buildup and standby drain on **ColorOS 17** ported to **Xiaomi 15 (`dada`, Snapdragon 8 Elite / SM8750)**.

---

## Exact Target Base Port

| Attribute | Value |
| :--- | :--- |
| **Target Device** | Xiaomi 15 (`dada`, Qualcomm SM8750 / `sun`, 12 GB RAM, BOLED 1–120 Hz LTPO) |
| **Ported OS Base** | **ColorOS 17 V6 CN — `PLZ110_17.0.0.107(CN01)`** (OnePlus 15T base, Android 17) |
| **Vendor / ODM Hardware Base** | Xiaomi 15 Stock HyperOS `OS4.0.0.11.XOCMIXM` (`vendor` / `odm` HALs & firmware) |
| **Optimization Build Milestone** | **`v7.2`** (100% rootless-baked `super.img`, zero `service.sh` runtime dependency) |
| **Companion GKI Kernel** | **`6.6.118-android15-wild-trsy` (`r24`)** ([`ampereforest/GKI_KernelSU_SUSFS`](https://github.com/ampereforest/GKI_KernelSU_SUSFS/releases/tag/r24)) |

> **Important for Porters:** All unified `.patch` files under [`patches/`](patches/) and AArch64 binary patchers under [`scripts/`](scripts/) are pinned against the exact base port above. Every ELF patcher verifies ELF headers, original MD5 hashes, and exact instruction bytes before modifying a single byte.

---

## Architectural Principles

Every optimization in this repository obeys five strict engineering rules:

1. **Zero Clock-Capping / Zero Performance Loss:** Never lower `scaling_max_freq` on CPU clusters (`3.53 GHz` Medium / `4.32 GHz` Prime) or Adreno GPU. Heat is eliminated by removing *wasted work* (polling loops, SELinux AVC storms, runaway thermal overrides, unneeded background wakeups, and `kswapd0` thrashing), not by handicapping peak performance.
2. **100% Rootless Architecture:** Every init trigger, property override, SELinux CIL rule, and pure-C oneshot helper is designed to bake directly into `super.img` with **zero root / zero Magisk or KernelSU `service.sh` dependency**.
3. **Zero Polling Loops:** Every custom daemon (`hdrpeakd`, `overlay.so`, `ltpofix_plugin.cpp`, `xmvibproxy_v2`, `cos17_thermal_once`, `cos17_dexopt_once`, `cos17_memkiller_ctl`) is either strictly **one-shot** or **kernel futex / AIDL / `CLOCK_MONOTONIC` condvar event-driven** (`0.000%` idle CPU).
4. **Zero Destructive Debloat:** No user-facing ColorOS apps or features are deleted. Background CN telemetry is contained non-destructively via Android 15 kernel eBPF (`FIREWALL_CHAIN_OEM_DENY_3`) + `appops`, while keeping `com.heytap.mydevices` active for Bluetooth UI and keeping Gallery AI (`com.oplus.aiunit` / `com.oplus.deepthinker`) and App Market (`com.heytap.market`) working on-demand (`0.0%` idle CPU when closed).
5. **Strict Enforcing SELinux Hygiene:** Zero DirtySepolicy rules, zero `*qemu*` properties, `0` new SELinux AVCs at boot/runtime, and `secilc -N` `NEW_PAIRS=0`.

---

## Key Benchmark Summary (`BEFORE` vs. `AFTER` on Bare Device, Zero External Fan)

| Test Scenario / Metric | Before (Base ColorOS 17 V6) | After (`v7.2` + `wild-trsy r24`) | Delta |
| :--- | :---: | :---: | :---: |
| **Screen-OFF 180s Cooldown — Battery Power** | `3,362 – 3,946 mW` | **`402 – 984 mW`** | **`-81%`** |
| **Screen-OFF 180s Cooldown — Battery Temp** | `39.3 °C` (stalled) | **`37.3 °C`** (`31.4 °C` clean idle) | **`-2.0 °C`** |
| **Screen-OFF 180s Cooldown — CPU Zones ≥ 50 °C** | `2 / 13` samples | **`0 / 13` samples** | **`-100%`** |
| **240s 120 Hz Light Usage — Settings Zones ≥ 50 °C** | `13.1` zones/sample | **`2.25` zones/sample** | **`-83%`** |
| **240s 120 Hz Light Usage — `kswapd0` CPU Time** | `20.72 s` | **`0.00 s`** | **`-100%`** |
| **240s 120 Hz Light Usage — `CPU0` `NET_RX` Softirqs** | `8,102` | **`798`** | **`-90.2%`** |
| **Settled Screen-OFF Total CPU Busy** | `~10–15%` + AVC/fork storms | **`1.78%`** (`98.22%` idle) | **98.2% idle** |
| **`oplus-color-mode-sync` (70-min settled)** | `8,854` jiffies (`2 Hz` fork) | **`0` jiffies** | **`-100%`** |
| **`libcameraopt.so` SELinux AVCs (30 min)** | `21,163+` denials | **`0` denials** | **`-100%`** |
| **15s Logcat Volume (Screen-ON / Screen-OFF)** | `1,581` / `142+` lines | **`267` / `65` lines** | **`-83%` ON** |
| **HDR Peak Daemon (`hdrpeakd`) Idle CPU** | `10.82%` (shell) / `98%` (spin bug) | **`0.000%`** (AIDL + futex) | **`-100%`** |
| **1–120 Hz Hardware LTPO Idle SF CPU** | `~4–6%` (`memfd` / `10 Hz` poll) | **`0.06%`** (`futex -> pipe2`) | **`-98%`** |
| **4K60 H.264 Browser Video — SurfaceFlinger CPU** | `251` jiffies / 5s (`6` blur layers) | **`44` jiffies / 5s** (`hwc_client=0`) | **`-82.5%`** |
| **4K60 H.264 Browser Video — Battery Power** | `3,819 mW` | **`2,618 mW`** | **`-1,201 mW (-31.4%)`** |
| **4K60 Video Under-Display CWB Triggers** | `39` / 5s (`200 ms`) | **`8` / 5s** (`1000 ms`) | **`-79.5%`** |
| **GKI Kernel KMI/ABI Drift (`android15-6.6`)** | Stock baseline (`9,317` symbols) | **`0` symbols differ** | **100% KMI-safe** |

Full raw tables and measurement methodology are documented in [`docs/BENCHMARKS-AND-TELEMETRY.md`](docs/BENCHMARKS-AND-TELEMETRY.md).

---

## Repository Layout

```text
.
├── README.md
├── docs/
│   ├── ROOT-CAUSES.md                         # Forensic analysis of all 7 categories + critical porter warnings
│   ├── BENCHMARKS-AND-TELEMETRY.md            # Complete BEFORE vs. AFTER telemetry across Passes 1–3 & v7.1–v7.2
│   └── KERNEL-GKI-TWEAKS.md                   # android15-6.6 KMI-safe power tweaks & runtime verification guide
├── patches/
│   ├── 01-thermal-perf/                       # PerfBreak neutralization, perfboostsconfig, perfconfigstore, sys_thermal_config, thermallevel_to_fps
│   ├── 02-display-ltpo-cwb/                   # CWB 1000ms throttle, LTPO/HDR props & deployment guide
│   ├── 03-telephony-wifi-rps/                 # WCNSS_qcom_cfg (ARP/APFv6 offload), init.rc hygiene, midasd/ostatsd rc, IMS/Dolby props, SELinux CIL
│   ├── 04-memory-vm-props/                    # post_boot WatermarkScale=20, system & vendor build.prop hygiene
│   └── 05-kernel-gki-defconfig/               # thermal-efficiency.fragment, GitHub Action, build.yml hook patch
├── src/
│   ├── daemons/                               # cos17_thermal_once.c, cos17_dexopt_once.c, cos17_memkiller_ctl.c, hdrpeakd.cpp, build_ndk.sh
│   ├── ltpo/                                  # overlay.cpp, ltpofix_plugin.cpp, Java LtpoFixHooks/PanelSafety/PointerMonitor
│   ├── haptics/                               # xmvibproxy_v2.cpp (Setup Wizard code=6 stop & single-envelope RTP)
│   ├── ui/                                    # VideoBlurSuppressionController.java (R14 smart video blur suppression)
│   └── init/                                  # init.cos17_dada_perf.rc, cos17_daemons.rc, cos17_dexopt.rc, hdrpeak.rc, ltpo_overlay.rc
└── scripts/
    ├── patch_surfaceflinger_renderintent.py   # Offset-verified ELF patcher: SurfaceFlinger RenderIntent translation
    ├── patch_color_mode_sync.py               # Offset-verified ELF patcher: eliminate 2 Hz settings fork storm
    ├── patch_pass2_culprits.py                # Offset-verified ELF patcher: libcameraopt, citsensorservice, libatlasservice, horae
    ├── patch_sensors_qsh_cct.py               # Offset-verified ELF patcher: sensors.qsh.so 0x1fa2672 -> 0x1fa2692
    └── patch_libopex.py                       # Offset-verified ELF patcher: libopex.so static opex bypass
```

---

## How Porters Apply These Fixes

### 1. Apply Unified Config, XML, Init & Property Patches (`patches/`)
From the root of your unpacked ROM tree (containing `system/`, `system_ext/`, `my_product/`, `my_stock/`, `vendor/`, `odm/`):

```bash
# 01 — Thermal HAL & Perf Moderation
patch -p1 < patches/01-thermal-perf/vendor_thermalbreakboostconfig.patch
cp patches/01-thermal-perf/odm_thermalbreakboostconfig.xml odm/etc/thermalbreakboostconfig.xml
patch -p1 < patches/01-thermal-perf/vendor_perfboostsconfig.patch
patch -p1 < patches/01-thermal-perf/vendor_perfconfigstore.patch
patch -p1 < patches/01-thermal-perf/odm_sys_thermal_config.patch
patch -p1 < patches/01-thermal-perf/my_stock_sys_thermal_config.patch
patch -p1 < patches/01-thermal-perf/vendor_thermallevel_to_fps.patch

# 02 — Display, LTPO & CWB
patch -p1 < patches/02-display-ltpo-cwb/vendor_cwb_display_props.patch

# 03 — Telephony, Wi-Fi Offload & System-Domain Init Hygiene
patch -p1 < patches/03-telephony-wifi-rps/vendor_wcnss_qcom_cfg.patch
patch -p1 < patches/03-telephony-wifi-rps/system_init_rc_hygiene.patch
patch -p1 < patches/03-telephony-wifi-rps/system_ext_midasd_rc.patch
patch -p1 < patches/03-telephony-wifi-rps/system_ext_ostatsd_rc.patch
patch -p1 < patches/03-telephony-wifi-rps/my_product_ims_audio_props.patch

# 04 — Memory VM & Rootless Build Properties
patch -p1 < patches/04-memory-vm-props/vendor_post_boot_memory_watermark.patch
patch -p1 < patches/04-memory-vm-props/system_build_prop_hygiene.patch
# Note: vendor_build_prop_thermal_hygiene.patch and vendor_cwb_display_props.patch
# both append to vendor/build.prop; merge the appended blocks if applying both.
```

Merge [`patches/03-telephony-wifi-rps/system_sepolicy_cos17_snippet.cil`](patches/03-telephony-wifi-rps/system_sepolicy_cos17_snippet.cil) into both `/system/etc/selinux/plat_sepolicy.cil` and `/system_ext/etc/selinux/plat_sepolicy_debug.cil`, validate with `secilc -N` (`NEW_PAIRS=0`), and update `/system/etc/selinux/plat_sepolicy_and_mapping.sha256`.

### 2. Compile Native C/C++ Zero-Polling Helpers & Install Init `.rc` Files (`src/`)
```bash
ANDROID_NDK_HOME=/path/to/android-ndk-r29 bash src/daemons/build_ndk.sh ./out/arm64-v8a
```
Install compiled binaries and `src/init/*.rc`:
- `cos17_thermal_once`, `cos17_dexopt_once`, `cos17_memkiller_ctl`, `hdrpeakd`, `xmvibproxy` -> `/vendor/bin/` (`0755`, matching `file_contexts` labels)
- `overlay.so` -> `/system/lib64/overlay.so` (`0644`)
- `src/init/{init.cos17_dada_perf,cos17_daemons,cos17_dexopt,hdrpeak}.rc` -> `/vendor/etc/init/` (`0644`)
- `src/init/ltpo_overlay.rc` -> `/system/etc/init/` (`0644`)

### 3. Run Offset-Verified AArch64 Binary Patchers (`scripts/`)
Every script checks ELF headers and exact original instructions before patching (use `--verify-only` first to dry-run):

```bash
python3 scripts/patch_surfaceflinger_renderintent.py -i system/bin/surfaceflinger -o out/surfaceflinger
python3 scripts/patch_color_mode_sync.py             -i odm/bin/hw/oplus-color-mode-sync -o out/oplus-color-mode-sync
python3 scripts/patch_sensors_qsh_cct.py             -i vendor/lib64/sensors.qsh.so -o out/sensors.qsh.so
python3 scripts/patch_libopex.py                     -i system_ext/lib64/libopex.so -o out/libopex.so
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

## Critical Pitfalls for Porters (Read Before Porting!)

1. **NEVER stop `vendor.diag-router` on SM8750 (`sun`):** Qualcomm `libdiag.so` enters a 100% CPU `error:111` busy-loop if the `diag-router` socket closes.
2. **NEVER block UID-1000 packages via `cmd connectivity set-package-networking-enabled false` (`OEM_DENY_3`):** Blocking UID-1000 packages (`com.heytap.mcs`, `com.oplus.obrain`, `com.oplus.midas`, `com.oplus.onetrace`) in `OEM_DENY_3` blocks UID `1000` (`system_server`) networking system-wide! Restrict UID-1000 packages via per-package `appops` **only**.
3. **Never put `system_prop` / `vendor_ims_prop` or `proc_watermark_scale_factor` writes in `vendor_init`:** Under Enforcing SELinux, `vendor_init` is denied `system_prop` / `vendor_ims_prop` writes. Place those triggers in `/system/etc/init/hw/init.rc` (`u:r:init:s0`) and set `WatermarkScale=20` inside `/vendor/bin/init.kernel.post_boot-memory.sh` + `cos17_thermal_once`.

---

## License

All custom C/C++/Java sources, Python patch scripts, init files, and documentation in this repository are licensed under the [MIT License](LICENSE). No proprietary vendor or OEM binaries are distributed in this repository.
