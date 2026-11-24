#!/usr/bin/env python3
"""
Offset-verified AArch64 ELF patcher for Pass-2 background wake/AVC culprits.

Target Base Port:
    ColorOS 17 V6 CN (PLZ110_17.0.0.107(CN01), OnePlus 15T base)
    ported to Xiaomi 15 (dada, SM8750) / HyperOS OS4.0.0.11 vendor/odm binaries.

Targets & Exact Verified Fixes:
    1) /vendor/lib64/libcameraopt.so
       - 0xd2e28: CameraCgroup::traceCgroupWorker() -> immediate `ret` (0xd65f03c0)
       - 0xd229c: CameraCgroup::eventCallBack() -> `b 0xd23bc` (0x14000048) skipping WatchDog::startMonitor()
       Result: Stops 24/7 `/dev/cpuctl/*/cpu.shares` SELinux AVC storm
       (21,163+ AVCs / 30 min -> 0) while preserving PerfLockManager camera boosts.

    2) /odm/bin/hw/vendor.xiaomi.sensor.citsensorservice.aidl
       - 0x10d8ec: skip waitForService("IHypSysSsi/default") -> `mov x0, xzr` (0xaa1f03e0)
         (stops 1 Hz 24/7 init/servicemanager wake storm on missing HyperOS service)
       - 0x136370: default persist.vendor.debug.collision.disable=true -> `mov w1, #1` (0x52800021)
       - 0xeeb74:  skip mCollision-detector 200 Hz WAKEUP sensor thread -> `nop` (0xd503201f)
       - 0xf265c, 0xf29ac, 0x12f498: silence 48 Hz BOLED/QSH INFO logcat floods -> `nop`

    3) /system_ext/lib64/libatlasservice.so
       - 0x143f0: skip OplusGpuMinidump 1 Hz missing-sysfs worker spawn -> `b 0x144a8` (0x1400002e)
         (placed AFTER sp<IAtlasService>::incStrong so refcount stays balanced)

    4) /system_ext/bin/horae
       - 0x3dd38: skip failing writeNode("/proc/shell-temp", ...) every 5 s -> `mov w0, #-1` (0x12800000)
         (preserves exact ENOENT return without failed open() syscall or E horae log spam)

Usage:
    python3 patch_pass2_culprits.py \
        --libcameraopt /path/to/orig/libcameraopt.so \
        --citsensor /path/to/orig/vendor.xiaomi.sensor.citsensorservice.aidl \
        --libatlasservice /path/to/orig/libatlasservice.so \
        --horae /path/to/orig/horae \
        --out-dir /path/to/out
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path


def enc_b(src_va: int, dst_va: int) -> int:
    diff = dst_va - src_va
    assert diff % 4 == 0, f"unaligned branch 0x{src_va:x} -> 0x{dst_va:x}"
    imm26 = diff >> 2
    assert -(1 << 25) <= imm26 < (1 << 25), f"branch out of range: {imm26}"
    return 0x14000000 | (imm26 & 0x03FFFFFF)


PATCH_SPECS = {
    "libcameraopt.so": {
        "orig_md5": "81fc513194ede0686c8fa9b79a05abd9",
        "patched_md5": "9461731a06fcd342af5294fb151b5cfa",
        "patches": [
            (0xD2E28, 0xD503233F, 0xD65F03C0, "CameraCgroup::traceCgroupWorker() -> immediate ret"),
            (0xD229C, 0x37000900, enc_b(0xD229C, 0xD23BC), "CameraCgroup::eventCallBack() skip WatchDog -> b 0xd23bc"),
        ],
    },
    "vendor.xiaomi.sensor.citsensorservice.aidl": {
        "orig_md5": "67fde372ec37475093f6552f18b124f2",
        "patched_md5": "b2da8ee0ac43cc04d6dc35d26919d3c6",
        "patches": [
            (0x10D8EC, 0x9400B523, 0xAA1F03E0, "Skip waitForService(IHypSysSsi) -> mov x0, xzr"),
            (0x136370, 0x2A1F03E1, 0x52800021, "Default persist.vendor.debug.collision.disable=1 -> mov w1, #1"),
            (0xEEB74, 0xD63F0100, 0xD503201F, "Skip mCollision-detector 200Hz WAKEUP thread -> nop"),
            (0xF265C, 0x940120A1, 0xD503201F, "Silence 48Hz libsensor-boledalgo dbv/lux INFO log -> nop"),
            (0xF29AC, 0x94011FCD, 0xD503201F, "Silence 48Hz libsensor-boledalgo lux/cct INFO log -> nop"),
            (0x12F498, 0x94002D12, 0xD503201F, "Silence 48Hz libsensor-qshcalapi set_lux INFO log -> nop"),
        ],
    },
    "libatlasservice.so": {
        "orig_md5": "54d0831e26e797c2137b24bbde6015f6",
        "patched_md5": "4c80220a8fae8fa0d95e223062049418",
        "patches": [
            (0x143F0, 0x94003D2A, enc_b(0x143F0, 0x144A8), "OplusGpuMinidump::init() skip pthread_create -> b 0x144a8"),
        ],
    },
    "horae": {
        "orig_md5": "15a0cd8a8e00a295aa7abd2141c10c48",
        "patched_md5": "74dfa25e353eee9226723ee5598a497d",
        "patches": [
            (0x3DD38, 0x97FFCEDF, 0x12800000, "Skip failing writeNode(/proc/shell-temp) -> mov w0, #-1"),
        ],
    },
}


def verify_elf_aarch64(data: bytes, name: str) -> None:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError(f"[{name}] Not a valid ELF binary")
    if data[4] != 2 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 183:
        raise RuntimeError(f"[{name}] Not AArch64 little-endian ELF")


def apply_spec(name: str, src: Path, dst: Path | None, verify_only: bool, allow_md5_mismatch: bool) -> None:
    spec = PATCH_SPECS[name]
    data = bytearray(src.read_bytes())
    verify_elf_aarch64(data, name)

    orig_md5 = hashlib.md5(data).hexdigest()
    print(f"[{name}] input MD5={orig_md5} ({len(data)} bytes)")
    if orig_md5 != spec["orig_md5"]:
        msg = f"[{name}] MD5 mismatch (expected {spec['orig_md5']})"
        if not allow_md5_mismatch:
            raise RuntimeError(msg)
        print(f"WARNING: {msg}; continuing with strict opcode verification.")

    for off, exp, _, desc in spec["patches"]:
        actual = struct.unpack_from("<I", data, off)[0]
        if actual != exp:
            raise RuntimeError(
                f"[{name}] Opcode mismatch at 0x{off:x} ({desc}): got 0x{actual:08x}, expected 0x{exp:08x}"
            )

    for off, exp, repl, desc in spec["patches"]:
        struct.pack_into("<I", data, off, repl)
        print(f"  0x{off:x}: 0x{exp:08x} -> 0x{repl:08x} ({desc})")

    out_md5 = hashlib.md5(data).hexdigest()
    print(f"[{name}] patched MD5={out_md5} (expected {spec['patched_md5']})")

    if not verify_only:
        assert dst is not None
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        print(f"[{name}] wrote {dst}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Patch Pass-2 background wake/AVC culprits")
    ap.add_argument("--libcameraopt", type=Path, help="Original /vendor/lib64/libcameraopt.so")
    ap.add_argument("--citsensor", type=Path, help="Original /odm/bin/hw/vendor.xiaomi.sensor.citsensorservice.aidl")
    ap.add_argument("--libatlasservice", type=Path, help="Original /system_ext/lib64/libatlasservice.so")
    ap.add_argument("--horae", type=Path, help="Original /system_ext/bin/horae")
    ap.add_argument("--out-dir", type=Path, help="Directory to write patched binaries")
    ap.add_argument("--verify-only", action="store_true", help="Verify offsets/opcodes without writing")
    ap.add_argument("--allow-md5-mismatch", action="store_true", help="Allow different build if exact opcodes match")
    args = ap.parse_args()

    selected = [
        ("libcameraopt.so", args.libcameraopt),
        ("vendor.xiaomi.sensor.citsensorservice.aidl", args.citsensor),
        ("libatlasservice.so", args.libatlasservice),
        ("horae", args.horae),
    ]
    selected = [(n, p) for n, p in selected if p is not None]
    if not selected:
        ap.error("Provide at least one target binary (--libcameraopt, --citsensor, --libatlasservice, or --horae)")
    if not args.verify_only and not args.out_dir:
        ap.error("--out-dir is required unless --verify-only is specified")

    for name, path in selected:
        dst = (args.out_dir / name) if args.out_dir else None
        apply_spec(name, path, dst, args.verify_only, args.allow_md5_mismatch)

    return 0


if __name__ == "__main__":
    sys.exit(main())
