#!/usr/bin/env bash
# Reproducible Android NDK build script for ColorOS 17 Xiaomi 15 (dada, arm64-v8a)
# zero-polling C/C++ helpers and LTPO libraries (v7.3.1, 16 KB ELF PT_LOAD aligned).
#
# Usage:
#   ANDROID_NDK_HOME=/path/to/android-ndk-r29 ./build_ndk.sh [out_dir]
#
# Optional (for PLZ110 /system/bin/surfaceflinger DT_NEEDED compatibility when replacing
# /system/lib64/overlay.so):
#   STATSLOG_BOOTSTRAP_SO=/path/to/libstatslog_surfaceflinger_bootstrap.so \
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
PAGE16_FLAG="-Wl,-z,max-page-size=16384"

echo "[1/6] Building cos17_thermal_once v8 (pure C, PIE, 16KB-aligned, zero libc++_shared)..."
"${CC}" -O2 -Wall -Wextra -Werror -fPIE -pie "${PAGE16_FLAG}" -s \
  "${SRC_ROOT}/daemons/cos17_thermal_once.c" \
  -o "${OUT_DIR}/cos17_thermal_once" -ldl

echo "[2/6] Building cos17_dexopt_once v2 (pure C, PIE, 16KB-aligned)..."
"${CC}" -O2 -Wall -Wextra -Werror -fPIE -pie "${PAGE16_FLAG}" -s \
  "${SRC_ROOT}/daemons/cos17_dexopt_once.c" \
  -o "${OUT_DIR}/cos17_dexopt_once" -ldl

echo "[3/6] Building cos17_memkiller_ctl (pure C, PIE, 16KB-aligned)..."
"${CC}" -O2 -Wall -Wextra -Werror -fPIE -pie "${PAGE16_FLAG}" -s \
  "${SRC_ROOT}/daemons/cos17_memkiller_ctl.c" \
  -o "${OUT_DIR}/cos17_memkiller_ctl" -ldl

echo "[4/6] Building hdrpeakd (C++17, static libc++, PIE, 16KB-aligned)..."
"${CXX}" -std=c++17 -O2 -Wall -Wextra -pthread -fPIE -pie "${PAGE16_FLAG}" -static-libstdc++ -s \
  "${SRC_ROOT}/daemons/hdrpeakd.cpp" \
  -o "${OUT_DIR}/hdrpeakd" -lbinder_ndk -ldl -llog

echo "[5/6] Building xmvibproxy (C++17, static libc++, PIE, 16KB-aligned)..."
"${CXX}" -std=c++17 -O2 -Wall -Wextra -pthread -fPIE -pie "${PAGE16_FLAG}" -static-libstdc++ -s \
  "${SRC_ROOT}/haptics/xmvibproxy_v2.cpp" \
  -o "${OUT_DIR}/xmvibproxy" -ldl -llog

echo "[6/6] Building overlay.so v4.1 (C++17, shared, BTI/PAC, 16KB-aligned, zero libc++_shared)..."
OVERLAY_EXTRA_LIBS=()
if [ -n "${STATSLOG_BOOTSTRAP_SO:-}" ]; then
  if [ ! -f "${STATSLOG_BOOTSTRAP_SO}" ]; then
    echo "ERROR: STATSLOG_BOOTSTRAP_SO not found: ${STATSLOG_BOOTSTRAP_SO}" >&2
    exit 1
  fi
  OVERLAY_EXTRA_LIBS+=(-Wl,--no-as-needed "${STATSLOG_BOOTSTRAP_SO}")
fi
"${CXX}" -std=c++17 -O2 -Wall -Wextra -pthread -fPIC -shared -mbranch-protection=standard \
  "${PAGE16_FLAG}" -static-libstdc++ -Wl,-soname,overlay.so -s \
  "${SRC_ROOT}/ltpo/overlay.cpp" \
  "${OVERLAY_EXTRA_LIBS[@]}" \
  -o "${OUT_DIR}/overlay.so" -ldl -llog

echo "Done. Built 16KB-aligned arm64-v8a binaries in: ${OUT_DIR}"
