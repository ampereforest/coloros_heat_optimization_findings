# Companion GKI Kernel Power-Efficiency Tweaks (`KERNEL-GKI-TWEAKS.md`)

**Companion Kernel Release:** `6.6.118-android15-wild-trsy` (`r24`)  
**Source Repository:** [`ampereforest/GKI_KernelSU_SUSFS`](https://github.com/ampereforest/GKI_KernelSU_SUSFS/releases/tag/r24) (commit [`86835f1`](https://github.com/ampereforest/GKI_KernelSU_SUSFS/commit/86835f1b2f0c7deb23f91013766393fb645d0429))  
**Target GKI Branch:** `android15-6.6` (4K page size, SM8750 / `dada` stock vendor modules)

---

## 1. Why KMI/ABI Safety Matters on Android 15 GKI

On Android 15 (`android15-6.6`), Xiaomi 15's proprietary vendor kernel modules (`vendor_dlkm` — Wi-Fi `qca_cld3_peach_v2`, GPU `msm_kgsl`, camera, display, IPA, and audio) are compiled against Google's frozen **Kernel Module Interface (KMI)** symbol table. Changing any `CONFIG_*` that alters a kernel struct layout, header definition, or exported symbol CRC breaks vendor module loading or fails the GKI ABI gate.

Our kernel power-efficiency fragment selects **strictly two boolean-initializer defaults** that change **zero headers, zero struct layouts, and zero exported symbols**:

```kconfig
# patches/05-kernel-gki-defconfig/thermal-efficiency.fragment
CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y
# CONFIG_RCU_LAZY_DEFAULT_OFF is not set
```

---

## 2. What Each Config Option Does

### 2.1 `CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y`
- **Kernel location:** `kernel/workqueue.c` (`static bool wq_power_efficient = IS_ENABLED(CONFIG_WQ_POWER_EFFICIENT_DEFAULT);`).
- **Behavior:** Routes kernel workqueues flagged `WQ_POWER_EFFICIENT` onto CPUs that are **already awake** rather than waking deep-idle cores out of C-states.
- **Performance impact:** Zero — latency-critical workqueues do not set `WQ_POWER_EFFICIENT` and continue running immediately on their local CPU.
- **Runtime sysfs toggle:** `/sys/module/workqueue/parameters/power_efficient` (`Y`).

### 2.2 `# CONFIG_RCU_LAZY_DEFAULT_OFF is not set`
- **Kernel location:** `kernel/rcu/tree.c` (`static bool enable_rcu_lazy = !IS_ENABLED(CONFIG_RCU_LAZY_DEFAULT_OFF);`).
- **Behavior:** Upstream `android15-6.6` `gki_defconfig` already compiles `CONFIG_RCU_LAZY=y`, but leaves it disabled at runtime via `CONFIG_RCU_LAZY_DEFAULT_OFF=y`. Clearing `CONFIG_RCU_LAZY_DEFAULT_OFF` activates lazy RCU callback batching by default on offloaded `rcu_nocbs` CPUs (Xiaomi 15 boots with `rcu_nocbs=0-7`), batching non-urgent `call_rcu()` callbacks to reduce idle timer/RCU wakeups while keeping urgent/synchronous RCU (`synchronize_rcu()`, `call_rcu_hurry()`) immediate.
- **Performance impact:** Zero — only defers non-urgent memory reclamation callbacks during idle periods.
- **Runtime sysfs toggle:** `/sys/module/rcutree/parameters/enable_rcu_lazy` (`Y`).

---

## 3. CI Build & KMI/ABI Gate Verification (`r24`)

The files under [`patches/05-kernel-gki-defconfig/`](../patches/05-kernel-gki-defconfig/) integrate cleanly into `GKI_KernelSU_SUSFS`:
- `thermal-efficiency.fragment` -> `.github/actions/thermal-efficiency/thermal-efficiency.fragment`
- `action.yml` -> `.github/actions/thermal-efficiency/action.yml` (scoped strictly to `inputs.version == 'android15-6.6'`)
- `build_yml_thermal_hook.patch` -> patches `.github/workflows/build.yml`

**Build Verification Results (GitHub Actions Run `45359611451` -> Release `r24`):**
- **Final `.config` Check:** Both `CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y` and `# CONFIG_RCU_LAZY_DEFAULT_OFF is not set` confirmed active in the built kernel configuration.
- **KMI / ABI Gate:** **`ABI gate: OK — 9,317 symbols compared against stock, 0 differ`**.
- **Vendor Module Compatibility:** All Xiaomi 15 (`dada`) `vendor_dlkm` modules (WCN7851 Wi-Fi, Adreno GPU, IPA, camera, audio, display) load cleanly without `insmod` errors or symbol version mismatches.

---

## 4. Runtime Verification Note (`/proc/config.gz` Spoofing Caveat)

> **Important Note on WildKernels `/proc/config.gz`:**  
> WildKernels builds intentionally embed a stock GKI `/proc/config.gz` snapshot for anti-detection hygiene. Therefore, `zcat /proc/config.gz` will still display the stock default (`CONFIG_RCU_LAZY_DEFAULT_OFF=y`) even when `enable_rcu_lazy` and `wq_power_efficient` are compiled and active in the running kernel binary!

Always verify active kernel behavior directly through the live kernel module parameters in `/sys/module/`:

```bash
uname -r
# Expected: 6.6.118-android15-wild-trsy

cat /sys/module/workqueue/parameters/power_efficient
# Expected: Y

cat /sys/module/rcutree/parameters/enable_rcu_lazy
# Expected: Y

grep -o 'rcu_nocbs=[^ ]*' /proc/cmdline
# Expected: rcu_nocbs=0-7
```

**Observed Live Result on Xiaomi 15 (`dada`, Clean-Flash `v7.2` + `wild-trsy r24`):**
- `uname -r`: `6.6.118-android15-wild-trsy`
- `/sys/module/workqueue/parameters/power_efficient`: **`Y`**
- `/sys/module/rcutree/parameters/enable_rcu_lazy`: **`Y`**
- `/proc/cmdline`: **`rcu_nocbs=0-7`**
- Settled idle battery temperature: **`31.4 °C`**
