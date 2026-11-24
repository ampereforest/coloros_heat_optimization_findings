#!/usr/bin/env python3
"""
Offset-verified AArch64 ELF patcher for /system/bin/surfaceflinger.

Target Base Port:
    ColorOS 17 V6 CN (PLZ110_17.0.0.107(CN01), OnePlus 15T base)
    ported to Xiaomi 15 (dada, SM8750)
    Original /system/bin/surfaceflinger MD5: 64d30afcf19272541941793eb774b05e
    Expected patched MD5:                    06aea02c14df2b312631654be8014d80

What this patch does:
    Moves ColorOS DisplayMode -> Xiaomi/QTI RenderIntent translation directly
    into SurfaceFlinger's three `str w*, [x*, #0x20e0]` mActiveColorMode write
    sites (BTI-safe code caves at 0x646120 and 0x646150):
        301 (0x12d, Natural) -> 257 (0x101, Xiaomi Standard)
        303 (0x12f, Vivid)   -> 258 (0x102, Xiaomi Vivid)
        307 (0x133, Pro)     -> 256 (0x100, Xiaomi Pro)

Usage:
    python3 patch_surfaceflinger_renderintent.py \
        --input /path/to/orig/surfaceflinger \
        --output /path/to/patched/surfaceflinger
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

EXPECTED_ORIG_MD5 = "64d30afcf19272541941793eb774b05e"
EXPECTED_PATCHED_MD5 = "06aea02c14df2b312631654be8014d80"

CAVE_1_OFFSET = 0x646120  # Translates w0, stores to [x19, #0x20e0], returns
CAVE_2_OFFSET = 0x646150  # Translates w19 (scratch w11), stores to [x20, #0x20e0], returns

# Helper 1 at 0x646120: translate w0 (301->257, 303->258, 307->256), store [x19, #0x20e0], ret
CAVE_1_INSNS = [
    0xD503245F,  # bti c
    0x7104B41F,  # cmp w0, #0x12d (301 Natural)
    0x52802028,  # mov w8, #0x101 (257 Xiaomi Standard)
    0x1A800100,  # csel w0, w8, w0, eq
    0x7104BC1F,  # cmp w0, #0x12f (303 Vivid)
    0x52802048,  # mov w8, #0x102 (258 Xiaomi Vivid)
    0x1A800100,  # csel w0, w8, w0, eq
    0x7104CC1F,  # cmp w0, #0x133 (307 Pro)
    0x52802008,  # mov w8, #0x100 (256 Xiaomi Pro)
    0x1A800100,  # csel w0, w8, w0, eq
    0xB920E260,  # str w0, [x19, #0x20e0]
    0xD65F03C0,  # ret
]

# Helper 2 at 0x646150: translate w19 (301->257, 303->258, 307->256) using dead w11, store [x20, #0x20e0], ret
CAVE_2_INSNS = [
    0xD503245F,  # bti c
    0x7104B67F,  # cmp w19, #0x12d (301 Natural)
    0x5280202B,  # mov w11, #0x101 (257 Xiaomi Standard)
    0x1A930173,  # csel w19, w11, w19, eq
    0x7104BE7F,  # cmp w19, #0x12f (303 Vivid)
    0x5280204B,  # mov w11, #0x102 (258 Xiaomi Vivid)
    0x1A930173,  # csel w19, w11, w19, eq
    0x7104CE7F,  # cmp w19, #0x133 (307 Pro)
    0x5280200B,  # mov w11, #0x100 (256 Xiaomi Pro)
    0x1A930173,  # csel w19, w11, w19, eq
    0xB920E293,  # str w19, [x20, #0x20e0]
    0xD65F03C0,  # ret
]

CALL_SITES = [
    (0x4FDA14, 0xB920E260, CAVE_1_OFFSET, "Site 1 (boot): str w0, [x19, #0x20e0] -> bl Cave1"),
    (0x288230, 0xB920E260, CAVE_1_OFFSET, "Site 2 (tx):   str w0, [x19, #0x20e0] -> bl Cave1"),
    (0x4FE948, 0xB920E293, CAVE_2_OFFSET, "Site 3 (int):  str w19, [x20, #0x20e0] -> bl Cave2"),
]


def encode_bl(pc: int, target: int) -> int:
    diff = target - pc
    if diff % 4 != 0:
        raise ValueError(f"Unaligned BL branch: {hex(pc)} -> {hex(target)}")
    imm26 = diff >> 2
    if not (-(1 << 25) <= imm26 < (1 << 25)):
        raise ValueError(f"BL target out of range: {hex(pc)} -> {hex(target)}")
    return 0x94000000 | (imm26 & 0x03FFFFFF)


def verify_elf_aarch64(data: bytes) -> None:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError("Input file is not a valid ELF binary")
    if data[4] != 2 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 183:
        raise RuntimeError("Input ELF is not AArch64 little-endian")


def patch_surfaceflinger(data: bytearray, allow_md5_mismatch: bool = False) -> bytearray:
    verify_elf_aarch64(data)
    orig_md5 = hashlib.md5(data).hexdigest()
    print(f"Input MD5: {orig_md5} ({len(data)} bytes)")
    if orig_md5 != EXPECTED_ORIG_MD5:
        msg = f"MD5 mismatch (expected {EXPECTED_ORIG_MD5} from PLZ110_17.0.0.107(CN01))"
        if not allow_md5_mismatch:
            raise RuntimeError(msg)
        print(f"WARNING: {msg}; continuing with strict opcode verification.")

    if data[0x646120:0x646180] != b"\x00" * 0x60:
        raise RuntimeError("Code cave 0x646120..0x646180 is not empty (already patched or wrong binary)")

    for off, exp, _, label in CALL_SITES:
        actual = struct.unpack_from("<I", data, off)[0]
        if actual != exp:
            raise RuntimeError(f"Opcode mismatch at 0x{off:x} ({label}): got 0x{actual:08x}, expected 0x{exp:08x}")

    for i, insn in enumerate(CAVE_1_INSNS):
        struct.pack_into("<I", data, CAVE_1_OFFSET + i * 4, insn)
    for i, insn in enumerate(CAVE_2_INSNS):
        struct.pack_into("<I", data, CAVE_2_OFFSET + i * 4, insn)

    for off, _, target, label in CALL_SITES:
        bl = encode_bl(off, target)
        struct.pack_into("<I", data, off, bl)
        print(f"  Patched 0x{off:x}: {label} (0x{bl:08x})")

    return data


def main() -> int:
    ap = argparse.ArgumentParser(description="Patch ColorOS 17 SurfaceFlinger RenderIntent translation")
    ap.add_argument("--input", "-i", required=True, type=Path, help="Original /system/bin/surfaceflinger")
    ap.add_argument("--output", "-o", type=Path, help="Output path for patched surfaceflinger")
    ap.add_argument("--verify-only", action="store_true", help="Verify offsets/opcodes without writing output")
    ap.add_argument("--allow-md5-mismatch", action="store_true", help="Allow different build if exact opcodes match")
    args = ap.parse_args()

    raw = bytearray(args.input.read_bytes())
    patched = patch_surfaceflinger(raw, allow_md5_mismatch=args.allow_md5_mismatch)
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
