#!/usr/bin/env bash
# Reproducible build script for split-scope rootless ColorOS 17 FCM Nyako plugins:
#   - libNyako_hook_fcmsys.so  (scope="android", 15 system_server rules)
#   - libNyako_hook_fcmbatt.so (scope="com.oplus.battery", 3 battery rules)
#
# Usage:
#   ANDROID_NDK_HOME=/path/to/android-ndk-r29 \
#   ANDROID_BUILD_TOOLS=/path/to/android-sdk/build-tools/37.0.0 \
#   LIBNYAKO_CORE_SO=/path/to/libnyako_core.so \
#   ./build_fcm.sh [out_dir]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/../out/fcm}"

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "ERROR: Set ANDROID_NDK_HOME to your Android NDK (r26-r29 recommended)." >&2
  exit 1
fi
if [ -z "${ANDROID_BUILD_TOOLS:-}" ] || [ ! -x "${ANDROID_BUILD_TOOLS}/d8" ]; then
  echo "ERROR: Set ANDROID_BUILD_TOOLS to an Android SDK build-tools directory containing d8." >&2
  exit 1
fi
if [ -z "${LIBNYAKO_CORE_SO:-}" ] || [ ! -f "${LIBNYAKO_CORE_SO}" ]; then
  echo "ERROR: Set LIBNYAKO_CORE_SO to your target ROM's libnyako_core.so (DT_NEEDED link target)." >&2
  exit 1
fi

JAVAC_BIN="${JAVA_HOME:+${JAVA_HOME}/bin/}javac"
JAR_BIN="${JAVA_HOME:+${JAVA_HOME}/bin/}jar"
LLVM="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin"
CXX="${LLVM}/aarch64-linux-android35-clang++"
STRIP="${LLVM}/llvm-strip"
READELF="${LLVM}/llvm-readelf"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p \
  "${WORK}/stubs_src/android/app" \
  "${WORK}/stubs_src/android/content/pm" \
  "${WORK}/stubs_src/android/os" \
  "${WORK}/stubs_src/android/util" \
  "${WORK}/stubs_src/com/android/server/alarm" \
  "${WORK}/stubs_src/com/android/server/am" \
  "${WORK}/stubs_src/com/android/server/hans" \
  "${WORK}/stubs_cls" "${WORK}/out_cls" "${WORK}/out_dex" \
  "${OUT_DIR}"

cat > "${WORK}/stubs_src/android/app/IApplicationThread.java" << 'EOF'
package android.app;
public interface IApplicationThread {}
EOF

cat > "${WORK}/stubs_src/android/app/PendingIntent.java" << 'EOF'
package android.app;
public final class PendingIntent {}
EOF

cat > "${WORK}/stubs_src/android/content/ComponentName.java" << 'EOF'
package android.content;
public final class ComponentName {
    public String getPackageName() { return null; }
}
EOF

cat > "${WORK}/stubs_src/android/content/IIntentReceiver.java" << 'EOF'
package android.content;
public interface IIntentReceiver {}
EOF

cat > "${WORK}/stubs_src/android/content/Intent.java" << 'EOF'
package android.content;
public class Intent {
    public String getAction() { return null; }
    public String getPackage() { return null; }
    public ComponentName getComponent() { return null; }
    public boolean getBooleanExtra(String name, boolean def) { return def; }
    public Intent putExtra(String name, boolean val) { return this; }
    public Intent addFlags(int flags) { return this; }
}
EOF

cat > "${WORK}/stubs_src/android/content/pm/ApplicationInfo.java" << 'EOF'
package android.content.pm;
public class ApplicationInfo {
    public int uid;
    public boolean enabled;
}
EOF

cat > "${WORK}/stubs_src/android/content/pm/PackageManager.java" << 'EOF'
package android.content.pm;
public abstract class PackageManager {
    public abstract int getPackageUid(String pkg, int flags) throws Exception;
    public abstract int getApplicationEnabledSetting(String pkg);
    public abstract ApplicationInfo getApplicationInfo(String pkg, int flags) throws Exception;
}
EOF

cat > "${WORK}/stubs_src/android/content/Context.java" << 'EOF'
package android.content;
import android.content.pm.PackageManager;
public abstract class Context {
    public abstract PackageManager getPackageManager();
}
EOF

cat > "${WORK}/stubs_src/android/app/Application.java" << 'EOF'
package android.app;
import android.content.Context;
public class Application extends Context {
    @Override public android.content.pm.PackageManager getPackageManager() { return null; }
}
EOF

cat > "${WORK}/stubs_src/android/app/ActivityThread.java" << 'EOF'
package android.app;
public final class ActivityThread {
    public static Application currentApplication() { return null; }
}
EOF

cat > "${WORK}/stubs_src/android/os/Binder.java" << 'EOF'
package android.os;
public class Binder {
    public static int getCallingUid() { return 0; }
}
EOF

cat > "${WORK}/stubs_src/android/os/Bundle.java" << 'EOF'
package android.os;
public final class Bundle {
    public String getString(String key) { return null; }
    public int getInt(String key, int def) { return def; }
}
EOF

cat > "${WORK}/stubs_src/android/os/SystemClock.java" << 'EOF'
package android.os;
public final class SystemClock {
    public static long elapsedRealtime() { return 0L; }
}
EOF

cat > "${WORK}/stubs_src/android/util/Log.java" << 'EOF'
package android.util;
public final class Log {
    public static int i(String t, String m) { return 0; }
    public static int w(String t, String m) { return 0; }
}
EOF

cat > "${WORK}/stubs_src/android/util/Pair.java" << 'EOF'
package android.util;
public class Pair<F, S> {
    public final F first = null;
    public final S second = null;
    public static <A, B> Pair<A, B> create(A a, B b) { return null; }
}
EOF

cat > "${WORK}/stubs_src/com/android/server/alarm/Alarm.java" << 'EOF'
package com.android.server.alarm;
import android.app.PendingIntent;
public class Alarm {
    public int type;
    public final boolean wakeup = false;
    public final PendingIntent operation = null;
    public final String listenerTag = null;
    public String statsTag;
    public final String packageName = null;
    public static String makeTag(PendingIntent op, String tag, int type) { return null; }
}
EOF

cat > "${WORK}/stubs_src/com/android/server/am/BroadcastQueue.java" << 'EOF'
package com.android.server.am;
public abstract class BroadcastQueue {}
EOF

cat > "${WORK}/stubs_src/com/android/server/am/BroadcastRecord.java" << 'EOF'
package com.android.server.am;
import android.content.Intent;
public final class BroadcastRecord {
    Intent intent;
    int callingUid;
}
EOF

cat > "${WORK}/stubs_src/com/android/server/am/OplusProxyBroadcast.java" << 'EOF'
package com.android.server.am;
public class OplusProxyBroadcast {
    public enum RESULT {
        NOT_INCLUDE,
        NOT_PROXY,
        PROXY
    }
}
EOF

cat > "${WORK}/stubs_src/com/android/server/hans/OplusHansPackage.java" << 'EOF'
package com.android.server.hans;
public class OplusHansPackage {
    public int getUid() { return -1; }
}
EOF

echo "[1/3] Compiling compile-time framework stubs..."
find "${WORK}/stubs_src" -name "*.java" -printf '"%p"\n' > "${WORK}/stubs_list.txt"
"${JAVAC_BIN}" --release 17 -d "${WORK}/stubs_cls" @"${WORK}/stubs_list.txt"
"${JAR_BIN}" cf "${WORK}/fcm_stubs.jar" -C "${WORK}/stubs_cls" .

echo "[2/3] Compiling NyakoBridge + FcmHooks -> fcm_classes.dex..."
"${JAVAC_BIN}" --release 17 -cp "${WORK}/fcm_stubs.jar" -d "${WORK}/out_cls" \
  "${SRC_ROOT}/ltpo/java/com/nyako/NyakoBridge.java" \
  "${SCRIPT_DIR}/java/com/nyako/fcm/FcmHooks.java"
"${JAR_BIN}" cf "${WORK}/fcm_in.jar" -C "${WORK}/out_cls" .
"${ANDROID_BUILD_TOOLS}/d8" --min-api 35 --lib "${WORK}/fcm_stubs.jar" \
  --output "${WORK}/out_dex" "${WORK}/fcm_in.jar"
cp "${WORK}/out_dex/classes.dex" "${WORK}/fcm_classes.dex"

cat > "${WORK}/embed_fcm_dex.S" << 'EOF'
.section .rodata, "a", @progbits
.global _binary_fcm_classes_dex_start
.global _binary_fcm_classes_dex_end
.balign 16
_binary_fcm_classes_dex_start:
.incbin "fcm_classes.dex"
_binary_fcm_classes_dex_end:
EOF

echo "[3/3] Linking 16KB-aligned split-scope libNyako_hook_fcmsys.so & libNyako_hook_fcmbatt.so..."
(
  cd "${WORK}"
  "${CXX}" -shared -fPIC -O2 \
    -DFCM_TARGET_BATTERY=0 \
    -mbranch-protection=standard -nostdlib++ \
    -Wl,-soname,libNyako_hook_fcmsys.so \
    -Wl,-z,max-page-size=16384 \
    -Wl,--no-as-needed \
    "${SCRIPT_DIR}/fcm_plugin.cpp" "${WORK}/embed_fcm_dex.S" \
    "${LIBNYAKO_CORE_SO}" -llog -lc -ldl \
    -o "${OUT_DIR}/libNyako_hook_fcmsys.so"
  "${STRIP}" --strip-unneeded "${OUT_DIR}/libNyako_hook_fcmsys.so"

  "${CXX}" -shared -fPIC -O2 \
    -DFCM_TARGET_BATTERY=1 \
    -mbranch-protection=standard -nostdlib++ \
    -Wl,-soname,libNyako_hook_fcmbatt.so \
    -Wl,-z,max-page-size=16384 \
    -Wl,--no-as-needed \
    "${SCRIPT_DIR}/fcm_plugin.cpp" "${WORK}/embed_fcm_dex.S" \
    "${LIBNYAKO_CORE_SO}" -llog -lc -ldl \
    -o "${OUT_DIR}/libNyako_hook_fcmbatt.so"
  "${STRIP}" --strip-unneeded "${OUT_DIR}/libNyako_hook_fcmbatt.so"
)

echo "Done. Built split-scope FCM plugins in: ${OUT_DIR}"
"${READELF}" -d -l "${OUT_DIR}/libNyako_hook_fcmsys.so" | grep -E "NEEDED|SONAME|LOAD" || true
"${READELF}" -d -l "${OUT_DIR}/libNyako_hook_fcmbatt.so" | grep -E "NEEDED|SONAME|LOAD" || true
