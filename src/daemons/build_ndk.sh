#!/usr/bin/env bash
# Reproducible Android NDK build script for ColorOS 17 Xiaomi 15 (dada, arm64-v8a)
# zero-polling C/C++ helpers and LTPO libraries.
#
# Usage:
#   ANDROID_NDK_HOME=/path/to/android-ndk-r29 ./build_ndk.sh [out_dir]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/../out/arm64-v8a}"

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "ERROR: Set ANDROID_NDK_HOME to your Android NDK (r26-r29 recommended)." >&2
  exit 1
fi

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="${TOOLCHAIN}/aarch64-linux-android35-clang"
CXX="${TOOLCHAIN}/aarch64-linux-android35-clang++"

if [ ! -x "${CC}" ] || [ ! -x "${CXX}" ]; then
  echo "ERROR: NDK API-35 clang toolchain not found under ${TOOLCHAIN}" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

echo "[1/6] Building cos17_thermal_once (pure C, PIE, zero libc++_shared)..."
"${CC}" -O2 -Wall -Wextra -Werror -fPIE -pie -s \
  "${SRC_ROOT}/daemons/cos17_thermal_once.c" \
  -o "${OUT_DIR}/cos17_thermal_once" -ldl

echo "[2/6] Building cos17_dexopt_once (pure C, PIE)..."
"${CC}" -O2 -Wall -Wextra -Werror -fPIE -pie -s \
  "${SRC_ROOT}/daemons/cos17_dexopt_once.c" \
  -o "${OUT_DIR}/cos17_dexopt_once" -ldl

echo "[3/6] Building cos17_memkiller_ctl (pure C, PIE)..."
"${CC}" -O2 -Wall -Wextra -Werror -fPIE -pie -s \
  "${SRC_ROOT}/daemons/cos17_memkiller_ctl.c" \
  -o "${OUT_DIR}/cos17_memkiller_ctl" -ldl

echo "[4/6] Building hdrpeakd (C++17, static libc++, PIE)..."
"${CXX}" -std=c++17 -O2 -Wall -Wextra -fPIE -pie -static-libstdc++ -s \
  "${SRC_ROOT}/daemons/hdrpeakd.cpp" \
  -o "${OUT_DIR}/hdrpeakd" -ldl -llog -lpthread

echo "[5/6] Building xmvibproxy (C++17, static libc++, PIE)..."
"${CXX}" -std=c++17 -O2 -Wall -Wextra -fPIE -pie -static-libstdc++ -s \
  "${SRC_ROOT}/haptics/xmvibproxy_v2.cpp" \
  -o "${OUT_DIR}/xmvibproxy" -ldl -llog -lpthread

echo "[6/6] Building overlay.so (C++17, shared, BTI/PAC, zero libc++_shared)..."
"${CXX}" -std=c++17 -O2 -Wall -Wextra -fPIC -shared -mbranch-protection=standard \
  -static-libstdc++ -Wl,-soname,overlay.so -s \
  "${SRC_ROOT}/ltpo/overlay.cpp" \
  -o "${OUT_DIR}/overlay.so" -ldl -llog -lpthread

echo "Done. Built binaries in: ${OUT_DIR}"
echo "Note: ltpofix_plugin.cpp embeds the compiled LtpoFixHooks DEX payload header"
echo "      (ltpofix_dex.h) generated via javac + d8 from src/ltpo/java/."
