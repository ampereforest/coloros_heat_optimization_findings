#!/usr/bin/env python3
"""
Offset-verified AArch64 ELF patcher for /system_ext/lib64/libopex.so.

Target Base Port:
    ColorOS 17 V6 CN (PLZ110_17.0.0.107(CN01), OnePlus 15T base)
    ported to Xiaomi 15 (dada, SM8750)
    Original /system_ext/lib64/libopex.so MD5: abae3c3ad25c662ebb54d45d54f0f5d3
    Expected patched MD5:                      fb70df7c338ab6c437253e1f7124ad18

What this patch does:
    When /system_ext/opex/*.opex payloads are statically unpacked into
    /system_ext/{framework/oplusex,lib64/oplusex,app,priv-app,etc}, this
    dual-offset string patch prevents `libopex.so` (`findOpexModules` /
    `startOpexMount`) from creating 6 runtime loop + overlayfs mounts at boot,
    eliminating redundant loop-device & overlayfs overhead while preserving
    /my_stock/opex (e.g. Omoji.opex).

    Exact offsets patched:
      - 0x0d671: NEON 16-byte SSO constructor chunk "/system_ext/ope" -> "/system_ext/op_"
      - 0x0f8ba: C-string literal "/system_ext/opex\\0"              -> "/system_ext/op_x\\0"

Usage:
    python3 patch_libopex.py \
        --input /path/to/orig/libopex.so \
        --output /path/to/patched/libopex.so
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

EXPECTED_ORIG_MD5 = "abae3c3ad25c662ebb54d45d54f0f5d3"
EXPECTED_PATCHED_MD5 = "fb70df7c338ab6c437253e1f7124ad18"

PATCH_SITES = [
    (0x0D671, b"/system_ext/ope", b"/system_ext/op_", "SSO constructor path chunk"),
    (0x0F8BA, b"/system_ext/opex\x00", b"/system_ext/op_x\x00", "startOpexMount C-string literal"),
]


def verify_elf_aarch64(data: bytes) -> None:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError("Input file is not a valid ELF binary")
    if data[4] != 2 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 183:
        raise RuntimeError("Input ELF is not AArch64 little-endian")


def patch_libopex(data: bytearray, allow_md5_mismatch: bool = False) -> bytearray:
    verify_elf_aarch64(data)
    orig_md5 = hashlib.md5(data).hexdigest()
    print(f"Input MD5: {orig_md5} ({len(data)} bytes)")
    if orig_md5 != EXPECTED_ORIG_MD5:
        msg = f"MD5 mismatch (expected {EXPECTED_ORIG_MD5} from PLZ110_17.0.0.107(CN01))"
        if not allow_md5_mismatch:
            raise RuntimeError(msg)
        print(f"WARNING: {msg}; continuing with strict string verification.")

    for off, exp, repl, desc in PATCH_SITES:
        assert len(exp) == len(repl)
        actual = bytes(data[off : off + len(exp)])
        if actual != exp:
            raise RuntimeError(f"Bytes mismatch at 0x{off:x} ({desc}): got {actual!r}, expected {exp!r}")

    for off, exp, repl, desc in PATCH_SITES:
        data[off : off + len(repl)] = repl
        print(f"  Patched 0x{off:x}: {exp!r} -> {repl!r} ({desc})")

    if b"/system_ext/ope" in data:
        raise RuntimeError("Residual /system_ext/ope reference found after patching!")

    return data


def main() -> int:
    ap = argparse.ArgumentParser(description="Bypass runtime /system_ext/opex loop/overlayfs mounts in libopex.so")
    ap.add_argument("--input", "-i", required=True, type=Path, help="Original /system_ext/lib64/libopex.so")
    ap.add_argument("--output", "-o", type=Path, help="Output path for patched libopex.so")
    ap.add_argument("--verify-only", action="store_true", help="Verify offsets/strings without writing")
    ap.add_argument("--allow-md5-mismatch", action="store_true", help="Allow different build if exact strings match")
    args = ap.parse_args()

    raw = bytearray(args.input.read_bytes())
    patched = patch_libopex(raw, allow_md5_mismatch=args.allow_md5_mismatch)
    out_md5 = hashlib.md5(patched).hexdigest()
    print(f"Patched MD5: {out_md5} (expected {EXPECTED_PATCHED_MD5})")

    if args.verify_only:
        print("Verify-only passed cleanly.")
        return 0

    if not args.output:
        ap.error("--output is required unless --verify-only is specified")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(patched)
    print(f"Wrote patched binary to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
