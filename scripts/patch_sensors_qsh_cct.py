#!/usr/bin/env python3
"""
Offset-verified AArch64 ELF patcher for /vendor/lib64/sensors.qsh.so.

Target Base Port:
    Xiaomi 15 (dada, SM8750) Stock HyperOS OS4.0.0.11 vendor base running
    ColorOS 17 V6 CN (PLZ110_17.0.0.107(CN01))
    Original /vendor/lib64/sensors.qsh.so MD5: 189d6ba206254e7d2967575680944658
    Expected patched MD5:                      c621187dba5a1b0d60de6bcfd06c61e5

What this patch does:
    Resolves a sensor type ID collision between OPPO and Xiaomi:
      - OPPO OplusExSystemService (`ExtendedDeviceService`) requests sensor type
        `0x01fa2672` (33171058), which on OPPO hardware is the MagSafe Car Holder
        Hall sensor.
      - On Xiaomi 15 QSH (`sensors.qsh.so`), `0x01fa2672` is `tcs3408 back CCT`
        (rear camera color-temperature sensor).
      - Because of the collision, OplusExSystemService registers `tcs3408 back CCT`
        at 5 Hz (200 ms) 24/7 even with screen OFF and spams `bad parameter`.
    Remaps the 3 `MOVZ w*, #0x2672` immediate loads (`0x01fa2672` -> `0x01fa2692`)
    without re-signing platform-signed OplusExSystemService.apk.

Usage:
    python3 patch_sensors_qsh_cct.py \
        --input /path/to/orig/sensors.qsh.so \
        --output /path/to/patched/sensors.qsh.so
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

EXPECTED_ORIG_MD5 = "189d6ba206254e7d2967575680944658"
EXPECTED_PATCHED_MD5 = "c621187dba5a1b0d60de6bcfd06c61e5"

PATCH_SITES = [
    # (file_offset, expected_orig_insn, replacement_insn, description)
    (0x03B1FC, 0x5284CE40, 0x5284D240, "MOVZ w0, #0x2672 -> MOVZ w0, #0x2692"),
    (0x070EA4, 0x5284CE49, 0x5284D249, "MOVZ w9, #0x2672 -> MOVZ w9, #0x2692"),
    (0x070EDC, 0x5284CE41, 0x5284D241, "MOVZ w1, #0x2672 -> MOVZ w1, #0x2692"),
]


def verify_elf_aarch64(data: bytes) -> None:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError("Input file is not a valid ELF binary")
    if data[4] != 2 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 183:
        raise RuntimeError("Input ELF is not AArch64 little-endian")


def patch_sensors_qsh(data: bytearray, allow_md5_mismatch: bool = False) -> bytearray:
    verify_elf_aarch64(data)
    orig_md5 = hashlib.md5(data).hexdigest()
    print(f"Input MD5: {orig_md5} ({len(data)} bytes)")
    if orig_md5 != EXPECTED_ORIG_MD5:
        msg = f"MD5 mismatch (expected {EXPECTED_ORIG_MD5})"
        if not allow_md5_mismatch:
            raise RuntimeError(msg)
        print(f"WARNING: {msg}; continuing with strict opcode verification.")

    for off, exp, _, desc in PATCH_SITES:
        actual = struct.unpack_from("<I", data, off)[0]
        if actual != exp:
            raise RuntimeError(f"Opcode mismatch at 0x{off:x} ({desc}): got 0x{actual:08x}, expected 0x{exp:08x}")

    for off, exp, repl, desc in PATCH_SITES:
        struct.pack_into("<I", data, off, repl)
        print(f"  Patched 0x{off:x}: 0x{exp:08x} -> 0x{repl:08x} ({desc})")

    return data


def main() -> int:
    ap = argparse.ArgumentParser(description="Remap tcs3408 back CCT ID in sensors.qsh.so (0x1fa2672 -> 0x1fa2692)")
    ap.add_argument("--input", "-i", required=True, type=Path, help="Original /vendor/lib64/sensors.qsh.so")
    ap.add_argument("--output", "-o", type=Path, help="Output path for patched sensors.qsh.so")
    ap.add_argument("--verify-only", action="store_true", help="Verify offsets/opcodes without writing")
    ap.add_argument("--allow-md5-mismatch", action="store_true", help="Allow different build if exact opcodes match")
    args = ap.parse_args()

    raw = bytearray(args.input.read_bytes())
    patched = patch_sensors_qsh(raw, allow_md5_mismatch=args.allow_md5_mismatch)
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
