#include <stddef.h>

/*
 * Pure freestanding PC-relative string-comparison scope matcher for ColorOS 17 ROM Nyako plugins.
 * Uses a single flat NUL-separated string blob (terminated by double-NUL) so AArch64 `adr`/`adrp`
 * resolves the table 100% PC-relatively at any ASLR load base with ZERO embedded pointers,
 * ZERO relocations, ZERO syscalls, and ZERO memory mapping.
 * Strips optional ":process_suffix" before matching against the static ROM scopes.
 */

static const char kRomScopesBlob[] =
    "android\0"
    "system_server\0"
    "systemui\0"
    "com.android.systemui\0"
    "com.android.settings\0"
    "com.oplus.battery\0"
    "com.oplus.aod\0"
    "com.coloros.phonemanager\0"
    "com.oplus.gesture\0"
    "com.miui.tsmclient\0"
    "com.xiaomi.account\0"
    "com.android.nfc\0"
    "com.coloros.oshare\0"
    "com.oplus.athena\0"
    "com.android.se\0"
    "com.android.camera\0"
    "com.oplus.exsystemservice\0"
    "com.oplus.ovoicemanager.wakeup\0";

__attribute__((section(".text.entry"), used, visibility("default")))
int nyako_is_rom_scope(const char *name) {
    if (!name || name[0] == '\0') {
        return 0;
    }

    size_t len = 0;
    while (name[len] != '\0' && name[len] != ':') {
        ++len;
    }
    if (len == 0) {
        return 0;
    }

    const char *p = kRomScopesBlob;
    while (*p != '\0') {
        size_t i = 0;
        while (p[i] != '\0' && i < len && p[i] == name[i]) {
            ++i;
        }
        if (i == len && p[i] == '\0') {
            return 1;
        }
        while (*p != '\0') {
            ++p;
        }
        ++p;
    }
    return 0;
}
