# Stability, Rootless Zero-Delay FCM & `QtiImsExtConnector` Deployment Notes (`v7.3` -> `v7.3.1`)

**Target Base Port:** ColorOS 17 V6 CN (`PLZ110_17.0.0.107(CN01)`, OnePlus 15T base) ported to Xiaomi 15 (`dada`, SM8750 / Stock HyperOS `OS4.0.0.11` vendor base).

---

## 1. Eliminating the `QtiImsExtConnector` Infinite Bind Storm (`v7.3.1` Fix #1)

- **Problem:** On ColorOS 17 V6 CN ported to Xiaomi 15, `org.codeaurora.ims` is **not installed** (VoLTE, VoNR, and 5G NR EN-DC run natively via Xiaomi/Qualcomm `imsdaemon` + `qcrilNrd`). However, `/product/framework/ims-ext-common.jar` (`org.codeaurora.ims.QtiImsExtConnector#bindQtiImsExtService()`) blindly calls `mContext.bindService(intent, mQtiImsExtServiceConnection, 1)` for `org.codeaurora.ims/.QtiImsExtService` and schedules a `500 ms` infinite retry loop (`mConnectionRetryHandler.sendEmptyMessageDelayed(1, 500)`) with zero backoff — generating **`11,734` failed `ActivityManager` bind attempts in 34 minutes** (`~6–10` Binder IPCs/sec forever) and holding screen-off idle CPU at `3.1–3.3%`.
- **Fix (`product_ims_ext_common_qti_connector.smali.patch`):**
  Adds a `PackageManager.resolveService(intent, 0) == null` check before `bindService()`: if `org.codeaurora.ims/.QtiImsExtService` is absent, it sets `mBound = false` and returns immediately without scheduling the retry handler.
- **Measured Result:** `11,734 -> 0` bind attempts; settled screen-off `Asleep` idle CPU dropped from **`3.1–3.3%` -> `0.79%`** (`-75%`) with Dual-SIM LTE + 5G NR EN-DC + VoLTE (`imsdaemon`) 100% intact.

### Exact Rebuild & Packing Checklist for `/product/framework/ims-ext-common.jar`
```bash
# 1. Disassemble stock ims-ext-common.jar
baksmali d product/framework/ims-ext-common.jar -o smali_out

# 2. Apply the unified smali patch
patch -d smali_out -p1 < patches/06-stability-fcm-ims/product_ims_ext_common_qti_connector.smali.patch

# 3. Reassemble classes.dex matching stock DEX version (dex\n039\x00 = API 28)
smali a -a 28 smali_out -o classes.dex

# 4. Verify resolveService bytecode via dexdump
dexdump -d classes.dex | grep -F "resolveService:(Landroid/content/Intent;I)Landroid/content/pm/ResolveInfo;"

# 5. Repack ims-ext-common.jar as 4-byte-aligned ZIP_STORED (ART mmap-ready)
#    preserving original META-INF/MANIFEST.MF (only classes.dex + META-INF/MANIFEST.MF).
```

> **CRITICAL EROFS Packing Requirement (Do Not Skip!):**  
> After replacing `/product/framework/ims-ext-common.jar`, you **MUST** delete the 6 stale pre-compiled oat/vdex/fs-verity files so ART does not load stale unpatched bytecode or fail checksum verification:
> - `product/framework/oat/arm/ims-ext-common.odex`
> - `product/framework/oat/arm/ims-ext-common.vdex`
> - `product/framework/oat/arm64/ims-ext-common.odex`
> - `product/framework/oat/arm64/ims-ext-common.odex.fsv_meta`
> - `product/framework/oat/arm64/ims-ext-common.vdex`
> - `product/framework/oat/arm64/ims-ext-common.vdex.fsv_meta`
>
> Remove those 6 entries from `packing/product/fs_config` and `packing/product/file_contexts` so partition parity remains `missing=0, extra=0`.

---

## 2. Rootless Zero-Delay FCM Push Architecture (`v7.3`)

ColorOS 17 CN restricts Google Play Services (`GmsCore`) FCM push delivery in **both** system XML feature flags and framework/battery Java hooks. Achieving instant Deep-Doze FCM wake without Magisk/KernelSU/LSPosed requires both layers:

### Layer A — System Feature & DeviceIdle Whitelist Patches
1. **`my_product_cn_gms_features.patch`:** Removes `<feature name="cn.google.services" />` from `/my_product/etc/permissions/oplus_google_cn_gms_features.xml` (while keeping `oplus.feature.control_cn_gms.xml` untouched so the Settings GMS switch continues working).
2. **`odm_gnss_bds_preference.patch`:** Removes the hidden second `<feature name="cn.google.services" />` injected by Xiaomi CN `/odm/etc/permissions/com.gnss.bds_preference.xml`. Missing this second file leaves `cn.google.services` active in `pm list features`!
3. **`my_region_deviceidle_whitelist.patch`:** Adds `<wl>com.google.android.gms</wl>` alongside `com.google.android.gsf` in `/my_region/etc/battery/sys_deviceidle_whitelist.xml`.

### Layer B — Split-Scope Native Nyako Plugins (`src/fcm/`)
Because `libnyako_core.so` matches `nyako_plugin_get_scope()` as a single exact package name, rootless FCM hooks are compiled into two 16 KB page-aligned shared libraries via [`src/fcm/build_fcm.sh`](../../src/fcm/build_fcm.sh):
- **`/system/lib64/libNyako_hook_fcmsys.so`** (`scope="android"`, 15 `system_server` hooks): rewrites `com.google.android.c2dm.intent.RECEIVE` from verified GMS caller UID to include `FLAG_INCLUDE_STOPPED_PACKAGES` (`0x20`), opens a leak-free bounded 32-slot `20 s` delivery window bypassing `OplusProxyBroadcast` / `OplusAppStartupManager` / `HansCGroup` freeze, blocks `OplusBgSceneManager` GMS restriction, restores downgraded GMS wakeup alarms in `OplusGoogleAlarmRestrict`, and protects GMS/GSF in `OplusNetworkManagementService` / `OAppNetControlService`.
- **`/system/lib64/libNyako_hook_fcmbatt.so`** (`scope="com.oplus.battery"`, 3 battery hooks): blocks `GoogleRestrictionController` (`POLICY_REJECT_ALL`), blocks `OplusNetworkingControlManager.setUidPolicy(uid, 4)` on Google core UIDs, and appends GMS/GSF to `ControllerCenter.determineNetWorkWhiteUidList()`.
- **100% Idempotent:** Coexists cleanly alongside optional LSPosed modules (`zopulus/fcmfix-coloros17`) with zero conflicts, and works natively on pure non-root clean flashes (`mtalk.google.com:5228` `ESTAB` inside forced `Deep Doze=IDLE`, `+3 ms` flag rewrite, `+18 ms` cold wake of force-stopped receiver).
- Remember to add both `.so` entries to `packing/system/fs_config` (`0 0 0644 capabilities=0x0`) and `packing/system/file_contexts` (`u:object_r:system_file:s0`).

---

## 3. Eliminating Post-Unlock Zygote Fork Stalls & SELinux AVC Storms (`libnyako_core.so` v7.3)

- **Problem:** Unpatched `libnyako_core.so` probed `/data/local/nyako/state/scopes` inside every forked user/GMS/KSU app, triggering `-EACCES` SELinux `avc: denied` storms and falling through into `nyako_scan_all_plugins()` (>10 MB ELF reads across 24 `.so` modules), while non-UID-0 system apps (`Camera`, `PhoneManager`) called `chmod()` on `root:root` `/data/local/nyako/{modules,state}` and `fsetxattr(nyako_data)` failed under Enforcing SELinux.
- **Fix:** Run [`scripts/patch_libnyako_scope.py`](../../scripts/patch_libnyako_scope.py) on v6.1 `libnyako_core.so` to embed the freestanding PC-relative 17-scope matcher ([`src/nyako/nyako_scope_filter.c`](../../src/nyako/nyako_scope_filter.c)) at `vaddr 0x190800` inside existing `PT_LOAD[4]`, gate directory `mkdir`/`chmod` on `geteuid() == 0`, and replace `fsetxattr@plt` hard-error sites with `mov w0, wzr`.
- **Measured Result:** Post-unlock `ActivityManager: Slow operation ... startProcess: returned from zygote!` stalls dropped **`122 -> 0`**, `nyako_avc = 0`, and `64/64` static RX pool pages (`256 KiB`) remain 100% free.

---

## 4. Signing Unsigned System RRO Overlays (`StrictJarManifest` Cleanliness)

On ColorOS 17 V6 CN, `/product/overlay/aon.frameworkres.overlay.product.apk` and `/my_product/overlay/aon.frameworkres.overlay.product.apk` ship completely unsigned (`Missing META-INF/MANIFEST.MF`), triggering `StrictJarManifest.read` `NullPointerException` warnings during `PackageManagerService` boot scans (including under CorePatch). Align (`zipalign -P 16 4`) and sign with v1+v2+v3 (deleting any `.idsig` sidecar before EROFS packing) to eliminate all boot-scan `StrictJarManifest` exceptions.
