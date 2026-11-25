#include <android/log.h>
#include <stdint.h>
#include <string.h>

#ifndef FCM_TARGET_BATTERY
#define FCM_TARGET_BATTERY 0
#endif

extern "C" {

struct NyakoRule {
    uint64_t reserved;
    const char* target_class;
    const char* hook_class;
    const char* method_name;
    const char* hook_sig;
};

int nyako_install_dex_data(const void* dex_data, uint32_t dex_size,
                           const NyakoRule* rules, uint32_t rule_count);

extern const uint8_t _binary_fcm_classes_dex_start[];
extern const uint8_t _binary_fcm_classes_dex_end[];

#if !FCM_TARGET_BATTERY
static const NyakoRule kSystemRules[] = {
    {
        0,
        "com.android.server.am.BroadcastController",
        "com.nyako.fcm.FcmHooks",
        "broadcastIntentWithFeature",
        "(Ljava/lang/Object;Landroid/app/IApplicationThread;Ljava/lang/String;Landroid/content/Intent;Ljava/lang/String;Landroid/content/IIntentReceiver;ILjava/lang/String;Landroid/os/Bundle;[Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;ILandroid/os/Bundle;ZZI)I",
    },
    {
        0,
        "com.android.server.hans.scene.OplusBgSceneManager",
        "com.nyako.fcm.FcmHooks",
        "isGmsRestricted",
        "(Ljava/lang/Object;)Z",
    },
    {
        0,
        "com.android.server.hans.scene.OplusBgSceneManager",
        "com.nyako.fcm.FcmHooks",
        "setGmsRestricted",
        "(Ljava/lang/Object;Z)V",
    },
    {
        0,
        "com.android.server.OplusDeviceIdleHelper",
        "com.nyako.fcm.FcmHooks",
        "getGoogleRestrictSwitch",
        "(Ljava/lang/Object;)Z",
    },
    {
        0,
        "com.android.server.am.OplusAppStartupManager$OplusStartupStrategy",
        "com.nyako.fcm.FcmHooks",
        "isGoogleRestricInfoOn",
        "(Ljava/lang/Object;I)Ljava/lang/Boolean;",
    },
    {
        0,
        "com.android.server.alarm.OplusGoogleAlarmRestrict",
        "com.nyako.fcm.FcmHooks",
        "updateGoogleAlarmTypeAndTag",
        "(Ljava/lang/Object;Lcom/android/server/alarm/Alarm;)V",
    },
    {
        0,
        "com.android.server.alarm.OplusDeepSleepHelper",
        "com.nyako.fcm.FcmHooks",
        "filterDeepSleepAlarm",
        "(Ljava/lang/Object;Lcom/android/server/alarm/Alarm;)Z",
    },
    {
        0,
        "com.android.server.net.OplusNetworkManagementService",
        "com.nyako.fcm.FcmHooks",
        "setFirewallUidRuleForNetworkType",
        "(Ljava/lang/Object;III)V",
    },
    {
        0,
        "com.android.server.nwpower.OAppNetControlService",
        "com.nyako.fcm.FcmHooks",
        "networkDisableWhiteList",
        "(Ljava/lang/Object;Ljava/util/List;I)I",
    },
    {
        0,
        "com.android.server.oplus.osense.OsenseResManagerService$3",
        "com.nyako.fcm.FcmHooks",
        "requestSceneActionSync",
        "(Ljava/lang/Object;Landroid/os/Bundle;)Z",
    },
    {
        0,
        "com.android.server.am.OplusProxyBroadcast",
        "com.nyako.fcm.FcmHooks",
        "shouldProxy",
        "(Ljava/lang/Object;Landroid/content/Intent;IILjava/lang/String;ILjava/lang/String;Ljava/lang/String;I)Lcom/android/server/am/OplusProxyBroadcast$RESULT;",
    },
    {
        0,
        "com.android.server.am.BroadcastProxyAction",
        "com.nyako.fcm.FcmHooks",
        "enqueueProxyBroadcastLocked",
        "(Ljava/lang/Object;ZLcom/android/server/am/BroadcastRecord;Ljava/lang/Object;Z)Z",
    },
    {
        0,
        "com.android.server.am.OplusAppStartupManager",
        "com.nyako.fcm.FcmHooks",
        "validStartProcessFromBroadcast",
        "(Ljava/lang/Object;Lcom/android/server/am/BroadcastQueue;Lcom/android/server/am/BroadcastRecord;Ljava/lang/String;I)Z",
    },
    {
        0,
        "com.android.server.nwpower.OAppNetControlService",
        "com.nyako.fcm.FcmHooks",
        "hansUpdateFirewallList",
        "(Ljava/lang/Object;Landroid/util/Pair;II)V",
    },
    {
        0,
        "com.android.server.hans.freeze.HansCGroup",
        "com.nyako.fcm.FcmHooks",
        "hansFreezeLocked",
        "(Ljava/lang/Object;Lcom/android/server/hans/OplusHansPackage;Ljava/lang/String;)Z",
    },
};
#else
static const NyakoRule kBatteryRules[] = {
    {
        0,
        "android.net.OplusNetworkingControlManager",
        "com.nyako.fcm.FcmHooks",
        "setUidPolicy",
        "(Ljava/lang/Object;II)V",
    },
    {
        0,
        "com.oplus.battery.restrictdynamicfeature.google.GoogleRestrictionController",
        "com.nyako.fcm.FcmHooks",
        "K",
        "(Ljava/lang/Object;ZZI)V",
    },
    {
        0,
        "com.oplus.deepsleep.ControllerCenter",
        "com.nyako.fcm.FcmHooks",
        "determineNetWorkWhiteUidList",
        "(Ljava/lang/Object;)Ljava/util/List;",
    },
};
#endif

static int matches_proc(const char* proc, const char* target) {
    if (!proc || !target) return 0;
    if (strcmp(proc, target) == 0) return 1;
    size_t len = strlen(target);
    if (strncmp(proc, target, len) == 0 && (proc[len] == ':' || proc[len] == '\0')) return 1;
    return 0;
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_name() {
#if !FCM_TARGET_BATTERY
    return "ColorOS 17 Rootless FCM System Push";
#else
    return "ColorOS 17 Rootless FCM Battery Guard";
#endif
}

__attribute__((visibility("default")))
int nyako_plugin_get_version() {
    return 2;
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_scope() {
#if !FCM_TARGET_BATTERY
    return "android";
#else
    return "com.oplus.battery";
#endif
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_desc() {
#if !FCM_TARGET_BATTERY
    return "Zero-delay rootless FCM push delivery & GMS wakeup/network protection (system_server)";
#else
    return "Zero-delay rootless FCM GMS network & restriction guard (com.oplus.battery)";
#endif
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_api() {
#if !FCM_TARGET_BATTERY
    return "android";
#else
    return "com.oplus.battery";
#endif
}

__attribute__((visibility("default")))
int nyako_plugin_entry(const char* proc_name) {
    uint32_t dex_size = static_cast<uint32_t>(_binary_fcm_classes_dex_end - _binary_fcm_classes_dex_start);
#if !FCM_TARGET_BATTERY
    if (matches_proc(proc_name, "android") || matches_proc(proc_name, "system_server")) {
        constexpr uint32_t n = sizeof(kSystemRules) / sizeof(kSystemRules[0]);
        int rc = nyako_install_dex_data(_binary_fcm_classes_dex_start, dex_size, kSystemRules, n);
        __android_log_print(ANDROID_LOG_INFO, "NyakoFcmFix", "system_server rules=%u rc=%d", n, rc);
        return rc;
    }
#else
    if (matches_proc(proc_name, "com.oplus.battery")) {
        constexpr uint32_t n = sizeof(kBatteryRules) / sizeof(kBatteryRules[0]);
        int rc = nyako_install_dex_data(_binary_fcm_classes_dex_start, dex_size, kBatteryRules, n);
        __android_log_print(ANDROID_LOG_INFO, "NyakoFcmFix", "battery rules=%u rc=%d", n, rc);
        return rc;
    }
#endif
    return 1;
}

} // extern "C"
