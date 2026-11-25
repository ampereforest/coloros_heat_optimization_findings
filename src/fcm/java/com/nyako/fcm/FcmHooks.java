package com.nyako.fcm;

import android.app.ActivityThread;
import android.app.Application;
import android.app.IApplicationThread;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.util.Pair;
import com.android.server.alarm.Alarm;
import com.android.server.am.BroadcastQueue;
import com.android.server.am.BroadcastRecord;
import com.android.server.am.OplusProxyBroadcast;
import com.android.server.hans.OplusHansPackage;
import com.nyako.NyakoBridge;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

/**
 * Leak-free, zero-thread, zero-Zygisk rootless ColorOS 17 FCM & GMS connectivity hook.
 *
 * Scopes:
 *   - "android" / "system_server" (slots 0..14)
 *   - "com.oplus.battery" (slots 0..2)
 *
 * Design invariants:
 *   - Zero background threads, executors, handlers, or file I/O.
 *   - Bounded fixed-size 32-slot ring buffer for 20s FCM delivery windows (zero heap growth).
 *   - Strict GMS caller UID attribution + enabled-package check before waking stopped targets.
 *   - 100% idempotent if zopulus/fcmfix-coloros17 is also active under LSPosed.
 */
public final class FcmHooks {
    private static final String TAG = "NyakoFcmFix";
    private static final String PKG_GMS = "com.google.android.gms";
    private static final String PKG_GSF = "com.google.android.gsf";
    private static final String PKG_VENDING = "com.android.vending";
    private static final String PKG_CFGUPD = "com.google.android.configupdater";
    private static final String ACTION_C2DM_RECEIVE = "com.google.android.c2dm.intent.RECEIVE";
    private static final String ACTION_FCM_EVENT = "com.google.firebase.MESSAGING_EVENT";
    private static final String ACTION_GOOGLE_RESTRICT = "oplus.intent.action.google_restrict_change";

    private static final int FLAG_INCLUDE_STOPPED_PACKAGES = 0x00000020;
    private static final long FCM_WINDOW_MS = 20_000L;
    private static final int RING_CAP = 32;

    private static final Object RING_LOCK = new Object();
    private static final int[] sWinUids = new int[RING_CAP];
    private static final String[] sWinPkgs = new String[RING_CAP];
    private static final long[] sWinExpMs = new long[RING_CAP];
    private static int sWinCursor = 0;

    private static volatile int sGmsAppId = -1;
    private static volatile int sGsfAppId = -1;
    private static volatile int sVendingAppId = -1;
    private static volatile int sCfgUpdAppId = -1;

    private static volatile Field sBrIntentField;
    private static volatile Field sBrCallingUidField;

    private FcmHooks() {}

    private static Context currentContext() {
        try {
            Application app = ActivityThread.currentApplication();
            return app;
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static void ensureGoogleAppIdsLoaded() {
        if (sGmsAppId > 0) return;
        Context ctx = currentContext();
        if (ctx == null) return;
        try {
            PackageManager pm = ctx.getPackageManager();
            if (pm == null) return;
            try { sGmsAppId = pm.getPackageUid(PKG_GMS, 0) % 100_000; } catch (Throwable ignored) {}
            try { sGsfAppId = pm.getPackageUid(PKG_GSF, 0) % 100_000; } catch (Throwable ignored) {}
            try { sVendingAppId = pm.getPackageUid(PKG_VENDING, 0) % 100_000; } catch (Throwable ignored) {}
            try { sCfgUpdAppId = pm.getPackageUid(PKG_CFGUPD, 0) % 100_000; } catch (Throwable ignored) {}
        } catch (Throwable ignored) {}
    }

    private static boolean isGmsCallerUid(int uid) {
        if (uid < 10_000) return false;
        ensureGoogleAppIdsLoaded();
        int appId = uid % 100_000;
        return sGmsAppId > 0 && appId == sGmsAppId;
    }

    private static boolean isGoogleCoreUid(int uid) {
        if (uid < 10_000) return false;
        ensureGoogleAppIdsLoaded();
        int appId = uid % 100_000;
        return (sGmsAppId > 0 && appId == sGmsAppId)
            || (sGsfAppId > 0 && appId == sGsfAppId)
            || (sVendingAppId > 0 && appId == sVendingAppId)
            || (sCfgUpdAppId > 0 && appId == sCfgUpdAppId);
    }

    private static String explicitTargetPackage(Intent intent) {
        if (intent == null) return null;
        ComponentName cn = intent.getComponent();
        if (cn != null) {
            String p = cn.getPackageName();
            if (p != null && !p.isEmpty()) return p;
        }
        String p = intent.getPackage();
        return (p != null && !p.isEmpty()) ? p : null;
    }

    private static boolean isFcmAction(String action) {
        return ACTION_C2DM_RECEIVE.equals(action) || ACTION_FCM_EVENT.equals(action);
    }

    private static int resolveEnabledTargetUid(String pkg, int userId) {
        if (pkg == null || pkg.isEmpty()) return -1;
        Context ctx = currentContext();
        if (ctx == null) return -1;
        try {
            PackageManager pm = ctx.getPackageManager();
            if (pm == null) return -1;
            int state = pm.getApplicationEnabledSetting(pkg);
            // Respect explicit user disable / IceBox freeze (2=DISABLED, 3=DISABLED_USER, 4=UNTIL_USED)
            if (state == 2 || state == 3 || state == 4) return -1;
            ApplicationInfo ai = pm.getApplicationInfo(pkg, 0);
            if (ai == null || !ai.enabled) return -1;
            int appId = ai.uid % 100_000;
            int u = (userId >= 0 && userId < 1000) ? userId : 0;
            return u * 100_000 + appId;
        } catch (Throwable ignored) {
            return -1;
        }
    }

    private static void recordDeliveryWindow(int uid, String pkg) {
        if (uid < 10_000 || pkg == null) return;
        long exp = SystemClock.elapsedRealtime() + FCM_WINDOW_MS;
        synchronized (RING_LOCK) {
            for (int i = 0; i < RING_CAP; i++) {
                if (sWinUids[i] == uid) {
                    sWinPkgs[i] = pkg;
                    sWinExpMs[i] = exp;
                    return;
                }
            }
            int idx = sWinCursor;
            sWinCursor = (idx + 1) & (RING_CAP - 1);
            sWinUids[idx] = uid;
            sWinPkgs[idx] = pkg;
            sWinExpMs[idx] = exp;
        }
    }

    private static boolean inDeliveryWindowUid(int uid) {
        if (uid < 10_000) return false;
        long now = SystemClock.elapsedRealtime();
        synchronized (RING_LOCK) {
            for (int i = 0; i < RING_CAP; i++) {
                if (sWinUids[i] == uid && sWinExpMs[i] > now) {
                    return true;
                }
            }
        }
        return false;
    }

    private static boolean inDeliveryWindowPkg(String pkg) {
        if (pkg == null || pkg.isEmpty()) return false;
        long now = SystemClock.elapsedRealtime();
        synchronized (RING_LOCK) {
            for (int i = 0; i < RING_CAP; i++) {
                if (sWinExpMs[i] > now && pkg.equals(sWinPkgs[i])) {
                    return true;
                }
            }
        }
        return false;
    }

    private static Intent getBroadcastRecordIntent(BroadcastRecord r) {
        if (r == null) return null;
        try {
            Field f = sBrIntentField;
            if (f == null) {
                f = BroadcastRecord.class.getDeclaredField("intent");
                f.setAccessible(true);
                sBrIntentField = f;
            }
            return (Intent) f.get(r);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static int getBroadcastRecordCallingUid(BroadcastRecord r) {
        if (r == null) return -1;
        try {
            Field f = sBrCallingUidField;
            if (f == null) {
                f = BroadcastRecord.class.getDeclaredField("callingUid");
                f.setAccessible(true);
                sBrCallingUidField = f;
            }
            return f.getInt(r);
        } catch (Throwable ignored) {
            return -1;
        }
    }

    private static void addUidStringIfAbsent(List<Object> list, int appId) {
        if (list == null || appId <= 0) return;
        String s = String.valueOf(appId);
        for (Object item : list) {
            if (item != null && s.equals(item.toString())) return;
        }
        list.add(s);
    }

    // =========================================================================
    // SYSTEM_SERVER HOOKS (Slots 0..14)
    // =========================================================================

    // Slot 0: BroadcastController.broadcastIntentWithFeature
    public static int broadcastIntentWithFeature(
            Object self, IApplicationThread caller, String callingFeatureId,
            Intent intent, String resolvedType, IIntentReceiver resultTo,
            int resultCode, String resultData, Bundle resultExtras,
            String[] requiredPermissions, String[] excludedPermissions,
            String[] excludedPackages, int appOp, Bundle bOptions,
            boolean serialized, boolean sticky, int userId) {
        try {
            if (intent != null) {
                String action = intent.getAction();
                if (ACTION_GOOGLE_RESTRICT.equals(action)
                        && intent.getBooleanExtra("restrict_enable", false)) {
                    intent.putExtra("restrict_enable", false);
                } else if (ACTION_C2DM_RECEIVE.equals(action)) {
                    int callingUid = Binder.getCallingUid();
                    if (isGmsCallerUid(callingUid)) {
                        String target = explicitTargetPackage(intent);
                        int targetUid = resolveEnabledTargetUid(target, userId);
                        if (targetUid > 0) {
                            intent.addFlags(FLAG_INCLUDE_STOPPED_PACKAGES);
                            recordDeliveryWindow(targetUid, target);
                        }
                    }
                }
            }
        } catch (Throwable ignored) {}
        return ((Integer) NyakoBridge.invokeOriginal(0, self, new Object[] {
            caller, callingFeatureId, intent, resolvedType, resultTo,
            resultCode, resultData, resultExtras, requiredPermissions,
            excludedPermissions, excludedPackages, appOp, bOptions,
            serialized, sticky, userId
        })).intValue();
    }

    // Slot 1: OplusBgSceneManager.isGmsRestricted()Z
    public static boolean isGmsRestricted(Object self) {
        return false;
    }

    // Slot 2: OplusBgSceneManager.setGmsRestricted(Z)V
    public static void setGmsRestricted(Object self, boolean restricted) {
        NyakoBridge.invokeOriginal(2, self, new Object[] { Boolean.FALSE });
    }

    // Slot 3: OplusDeviceIdleHelper.getGoogleRestrictSwitch()Z
    public static boolean getGoogleRestrictSwitch(Object self) {
        return false;
    }

    // Slot 4: OplusAppStartupManager$OplusStartupStrategy.isGoogleRestricInfoOn(I)Ljava/lang/Boolean;
    public static Boolean isGoogleRestricInfoOn(Object self, int userId) {
        return Boolean.FALSE;
    }

    // Slot 5: OplusGoogleAlarmRestrict.updateGoogleAlarmTypeAndTag(Alarm)V
    public static void updateGoogleAlarmTypeAndTag(Object self, Alarm alarm) {
        try {
            if (alarm != null && alarm.wakeup) {
                int curType = alarm.type;
                if (curType == 1 || curType == 3) {
                    int restored = curType - 1;
                    alarm.type = restored;
                    alarm.statsTag = Alarm.makeTag(alarm.operation, alarm.listenerTag, restored);
                }
            }
        } catch (Throwable ignored) {}
    }

    // Slot 6: OplusDeepSleepHelper.filterDeepSleepAlarm(Alarm)Z
    public static boolean filterDeepSleepAlarm(Object self, Alarm alarm) {
        boolean filtered = ((Boolean) NyakoBridge.invokeOriginal(6, self, new Object[] { alarm })).booleanValue();
        if (!filtered || alarm == null) return filtered;
        try {
            if (alarm.wakeup && (PKG_GMS.equals(alarm.packageName) || PKG_GSF.equals(alarm.packageName))) {
                return false;
            }
        } catch (Throwable ignored) {}
        return filtered;
    }

    // Slot 7: OplusNetworkManagementService.setFirewallUidRuleForNetworkType(III)V
    public static void setFirewallUidRuleForNetworkType(Object self, int netType, int uid, int rule) {
        int effectiveRule = (rule == 2 && isGoogleCoreUid(uid)) ? 1 : rule;
        NyakoBridge.invokeOriginal(7, self, new Object[] { netType, uid, effectiveRule });
    }

    // Slot 8: OAppNetControlService.networkDisableWhiteList(List, I)I
    @SuppressWarnings("unchecked")
    public static int networkDisableWhiteList(Object self, List list, int enable) {
        List effective = list;
        try {
            if (enable != 1 && list != null) {
                ensureGoogleAppIdsLoaded();
                ArrayList<Object> copy = new ArrayList<Object>(list);
                addUidStringIfAbsent(copy, sGmsAppId);
                addUidStringIfAbsent(copy, sGsfAppId);
                effective = copy;
            }
        } catch (Throwable ignored) {}
        return ((Integer) NyakoBridge.invokeOriginal(8, self, new Object[] { effective, enable })).intValue();
    }

    // Slot 9: OsenseResManagerService$3.requestSceneActionSync(Bundle)Z
    public static boolean requestSceneActionSync(Object self, Bundle bundle) {
        try {
            if (bundle != null
                    && PKG_GMS.equals(bundle.getString("pkgName"))
                    && "DeepSleepLogicDisNetRestore".equals(bundle.getString("reason"))
                    && bundle.getInt("strategy", -1) == 2
                    && "SCENE_COMMON_REQUEST".equals(bundle.getString("action"))
                    && "CommonExternalClean".equals(bundle.getString("subAction"))
                    && "clean".equals(bundle.getString("policy"))
                    && "com.oplus.battery".equals(bundle.getString("caller_package"))) {
                return false;
            }
        } catch (Throwable ignored) {}
        return ((Boolean) NyakoBridge.invokeOriginal(9, self, new Object[] { bundle })).booleanValue();
    }

    // Slot 10: OplusProxyBroadcast.shouldProxy
    public static OplusProxyBroadcast.RESULT shouldProxy(
            Object self, Intent intent, int callingUid, int callingPid,
            String resolvedType, int receivingUid, String callerPkg, String targetPkg, int userId) {
        try {
            if (intent != null && isFcmAction(intent.getAction())) {
                String tgt = (targetPkg != null && !targetPkg.isEmpty()) ? targetPkg : explicitTargetPackage(intent);
                if (isGmsCallerUid(callingUid) && (inDeliveryWindowUid(receivingUid) || inDeliveryWindowPkg(tgt))) {
                    return OplusProxyBroadcast.RESULT.NOT_INCLUDE;
                }
            }
        } catch (Throwable ignored) {}
        return (OplusProxyBroadcast.RESULT) NyakoBridge.invokeOriginal(10, self, new Object[] {
            intent, callingUid, callingPid, resolvedType, receivingUid, callerPkg, targetPkg, userId
        });
    }

    // Slot 11: BroadcastProxyAction.enqueueProxyBroadcastLocked
    public static boolean enqueueProxyBroadcastLocked(
            Object self, boolean isParallel, BroadcastRecord r, Object receiver, boolean Ordered) {
        try {
            Intent intent = getBroadcastRecordIntent(r);
            if (intent != null && isFcmAction(intent.getAction()) && isGmsCallerUid(getBroadcastRecordCallingUid(r))) {
                String tgt = explicitTargetPackage(intent);
                if (inDeliveryWindowPkg(tgt)) {
                    return false;
                }
            }
        } catch (Throwable ignored) {}
        return ((Boolean) NyakoBridge.invokeOriginal(11, self, new Object[] {
            isParallel, r, receiver, Ordered
        })).booleanValue();
    }

    // Slot 12: OplusAppStartupManager.validStartProcessFromBroadcast
    public static boolean validStartProcessFromBroadcast(
            Object self, BroadcastQueue queue, BroadcastRecord r, String targetPkg, int flag) {
        try {
            Intent intent = getBroadcastRecordIntent(r);
            if (intent != null && ACTION_C2DM_RECEIVE.equals(intent.getAction())
                    && isGmsCallerUid(getBroadcastRecordCallingUid(r))) {
                String tgt = (targetPkg != null && !targetPkg.isEmpty()) ? targetPkg : explicitTargetPackage(intent);
                if (inDeliveryWindowPkg(tgt)) {
                    return false;
                }
            }
        } catch (Throwable ignored) {}
        return ((Boolean) NyakoBridge.invokeOriginal(12, self, new Object[] {
            queue, r, targetPkg, flag
        })).booleanValue();
    }

    // Slot 13: OAppNetControlService.hansUpdateFirewallList(Pair, I, I)V
    public static void hansUpdateFirewallList(Object self, Pair update, int a1, int a2) {
        Pair effective = update;
        try {
            if (update != null && (update.first instanceof Integer) && (update.second instanceof Boolean)) {
                int uid = ((Integer) update.first).intValue();
                boolean restore = ((Boolean) update.second).booleanValue();
                if (!restore && inDeliveryWindowUid(uid)) {
                    effective = Pair.create(Integer.valueOf(uid), Boolean.TRUE);
                }
            }
        } catch (Throwable ignored) {}
        NyakoBridge.invokeOriginal(13, self, new Object[] { effective, a1, a2 });
    }

    // Slot 14: HansCGroup.hansFreezeLocked(OplusHansPackage, String)Z
    public static boolean hansFreezeLocked(Object self, OplusHansPackage pkg, String reason) {
        try {
            if (pkg != null && inDeliveryWindowUid(pkg.getUid())) {
                return false;
            }
        } catch (Throwable ignored) {}
        return ((Boolean) NyakoBridge.invokeOriginal(14, self, new Object[] { pkg, reason })).booleanValue();
    }

    // =========================================================================
    // COM.OPLUS.BATTERY HOOKS (Slots 0..2 in com.oplus.battery ClassLoader)
    // =========================================================================

    // Battery Slot 0: OplusNetworkingControlManager.setUidPolicy(II)V
    public static void setUidPolicy(Object self, int uid, int policy) {
        int effectivePolicy = (policy == 4 && isGoogleCoreUid(uid)) ? 0 : policy;
        NyakoBridge.invokeOriginal(0, self, new Object[] { uid, effectivePolicy });
    }

    // Battery Slot 1: GoogleRestrictionController.K(ZZI)V
    public static void K(Object self, boolean restrictEnable, boolean listChange, int reason) {
        NyakoBridge.invokeOriginal(1, self, new Object[] { Boolean.FALSE, listChange, reason });
    }

    // Battery Slot 2: ControllerCenter.determineNetWorkWhiteUidList()Ljava/util/List;
    @SuppressWarnings("unchecked")
    public static List determineNetWorkWhiteUidList(Object self) {
        List orig = (List) NyakoBridge.invokeOriginal(2, self, new Object[0]);
        try {
            if (orig != null) {
                ensureGoogleAppIdsLoaded();
                ArrayList<Object> copy = new ArrayList<Object>(orig);
                addUidStringIfAbsent(copy, sGmsAppId);
                addUidStringIfAbsent(copy, sGsfAppId);
                return copy;
            }
        } catch (Throwable ignored) {}
        return orig;
    }
}
