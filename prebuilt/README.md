# Prebuilt Custom Binaries & Runtime Hooks (`v7.3.1`)

All binaries in this directory are **16 KiB-aligned ARM64 (`aarch64`)** artifacts extracted directly from our verified pure non-root **`v7.3.1`** ColorOS 17 build on Xiaomi 15 (`dada`, SM8750). You can either drop them directly into the target paths below or rebuild them from `src/` + `scripts/`.

Verify checksums before integration:
```bash
cd prebuilt && sha256sum -c SHA256SUMS
```

| File | Target ROM Path | Mode | SELinux Context | Role |
| :--- | :--- | :---: | :--- | :--- |
| `system/lib64/libnyako_core.so` | `/system/lib64/libnyako_core.so` | `0644` | `u:object_r:system_file:s0` | Step A (`v6.1` zero-`execmem` 256 KiB `PT_LOAD[4]` COW pool) + Step B (`v7.3` 17-scope fast-path, UID-0 `chmod` gate, `fsetxattr` no-op; MD5 `50eefaa82d4062aecb08a09111f64d5b`) |
| `system/lib64/overlay.so` | `/system/lib64/overlay.so` | `0644` | `u:object_r:system_file:s0` | `v4.1` zero-polling hardware 1–120 Hz LTPO hook + `RefreshRateSelector::setPolicy` `SIGABRT` soft-reboot pre-guard (`MD5 8da86b168a45c0b86fbcec32ab0f2df3`) |
| `system/lib64/libNyako_hook_ltpofix.so` | `/system/lib64/libNyako_hook_ltpofix.so` | `0644` | `u:object_r:system_file:s0` | `v8.1` `pipe2()` event-driven LTPO hook + post-Doze wake resync (`MD5 f38c789f4cfdb3fb93860d1d8d1da53f`) |
| `system/lib64/libNyako_hook_fcmsys.so` | `/system/lib64/libNyako_hook_fcmsys.so` | `0644` | `u:object_r:system_file:s0` | Rootless zero-delay FCM plugin (`scope="android"`, 15 hooks; `MD5 b4eba74517c4f2134796b69f4ac64e5e`) |
| `system/lib64/libNyako_hook_fcmbatt.so` | `/system/lib64/libNyako_hook_fcmbatt.so` | `0644` | `u:object_r:system_file:s0` | Rootless zero-delay FCM plugin (`scope="com.oplus.battery"`, 3 hooks; `MD5 ae3ded6df1ee905f1c44c2ca10f02c05`) |
| `system/etc/mifod/fod-bridge.jar` | `/system/etc/mifod/fod-bridge.jar` | `0644` | `u:object_r:system_file:s0` | Patched `MiFodBridge` with cached `IPowerManager`/`ITrustManager` binders & debug logging off (`MD5 d0036036809d35551729b33a6ea2b17e`) |
| `vendor/bin/cos17_thermal_once` | `/vendor/bin/cos17_thermal_once` | `0755` | `u:object_r:cos17_memkiller_exec:s0` | `v8` rootless pure-C boot helper: one-time post-reset defaults (`cos17_defaults_v1=1`), `HYGIENE_VER=75` AppOps pass, per-boot volatile `OEM_DENY_3` (`MD5 82932939b7a8fefb868971047dbdc565`) |
| `vendor/bin/cos17_dexopt_once` | `/vendor/bin/cos17_dexopt_once` | `0755` | `u:object_r:cos17_memkiller_exec:s0` | `v2` (`DEXOPT_VER=75`) post-CE-unlock ART AOT compiler (`SystemUI` + `SmartSideBar` `-m speed`, 8 core apps `-m speed-profile`; `MD5 9647e347c528336726e0b8216aa8daf3`) |
| `vendor/bin/cos17_memkiller_ctl` | `/vendor/bin/cos17_memkiller_ctl` | `0755` | `u:object_r:cos17_memkiller_exec:s0` | One-shot native LMKD/minfree controller backing Developer Options `Disable memory killer` (`MD5 352ec60f7feaf1dd51fb23b6ce14de90`) |
| `vendor/bin/hdrpeakd` | `/vendor/bin/hdrpeakd` | `0755` | `u:object_r:hdrpeak_exec:s0` | Zero-polling `0.000%` idle C++ `IHdrLayerInfoListener` + `IDisplayFeature` 3200-nit HDR peak daemon (`MD5 07b7169db38a8004027aae229bc67da8`) |
| `vendor/bin/hw/xmvibproxy` | `/vendor/bin/hw/xmvibproxy` | `0755` | `u:object_r:hal_vibrator_default_exec:s0` | Event-driven AIDL `IVibrator` + `IOplusVibrator` extension forwarding & RichTap HE 1.0/2.0 proxy (`MD5 480961818ec370490f3689a8158e3083`) |
| `vendor/bin/xtouch_tune` | `/vendor/bin/xtouch_tune` | `0755` | `u:object_r:vendor_xtouch_tune_exec:s0` | DT2W (`mode 14`) & touch responsiveness tuner preserving `NONUI` pocket protection (`MD5 542f5c1057231d56f5c3cc666d3e62f2`) |

> **Note on `libnyako_core.so`:** If rebuilding from base ColorOS 17 V6 `libnyako_core.so` (`MD5 c63b3763c7949a13fc8d67dfbaf2021f`), run `scripts/patch_libnyako_core.py` (Step A, zero-`execmem`) followed by `scripts/patch_libnyako_scope.py` (Step B, 17-scope fast-path). Both steps are already applied in `prebuilt/system/lib64/libnyako_core.so`.
