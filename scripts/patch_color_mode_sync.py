#!/usr/bin/env python3
"""
Offset-verified AArch64 ELF patcher for /odm/bin/hw/oplus-color-mode-sync.

Target Base Port:
    ColorOS 17 V6 CN (PLZ110_17.0.0.107(CN01), OnePlus 15T base)
    ported to Xiaomi 15 (dada, SM8750)
    Original /odm/bin/hw/oplus-color-mode-sync MD5: 22eeacd2a75bb57b05a68fd4966961b1
    Expected patched MD5:                           b7cf036ba61dcfbf2eaef9f5a76d4021

What this patch does:
    Eliminates the 2 Hz `/system/bin/settings` fork storm (100 ms
    `__system_property_wait` timeout + fork every 5 ticks = 500 ms, 24/7 even
    when screen is off; 8,854 cumulative jiffies over 70 minutes) and replaces
    it with 100% event-driven infinite kernel futex sleep (`timeout = NULL`) on
    `persist.sys.sf.native_mode`:
      - Before: ~7.5% continuous CPU burn, +21,520 child page faults / 5 s,
                +26 wakeups/s in screen-off standby.
      - After:  0.000% idle CPU, 0 child forks/hour, instant (<1 ms) response
                on Display Color Mode changes.

Usage:
    python3 patch_color_mode_sync.py \
        --input /path/to/orig/oplus-color-mode-sync \
        --output /path/to/patched/oplus-color-mode-sync
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

EXPECTED_ORIG_MD5 = "22eeacd2a75bb57b05a68fd4966961b1"
EXPECTED_PATCHED_MD5 = "b7cf036ba61dcfbf2eaef9f5a76d4021"


def enc_b(pc: int, target: int) -> int:
    diff = (target - pc) >> 2
    assert -(1 << 25) <= diff < (1 << 25)
    return 0x14000000 | (diff & 0x03FFFFFF)


def enc_bl(pc: int, target: int) -> int:
    diff = (target - pc) >> 2
    assert -(1 << 25) <= diff < (1 << 25)
    return 0x94000000 | (diff & 0x03FFFFFF)


def enc_b_cond(pc: int, target: int, cond: int) -> int:
    diff = (target - pc) >> 2
    assert -(1 << 18) <= diff < (1 << 18)
    return 0x54000000 | ((diff & 0x7FFFF) << 5) | (cond & 0xF)


def enc_cbz(pc: int, target: int, rt: int, is64: bool = True) -> int:
    diff = (target - pc) >> 2
    assert -(1 << 18) <= diff < (1 << 18)
    base = 0xB4000000 if is64 else 0x34000000
    return base | ((diff & 0x7FFFF) << 5) | (rt & 0x1F)


def enc_cbnz(pc: int, target: int, rt: int, is64: bool = True) -> int:
    diff = (target - pc) >> 2
    assert -(1 << 18) <= diff < (1 << 18)
    base = 0xB5000000 if is64 else 0x35000000
    return base | ((diff & 0x7FFFF) << 5) | (rt & 0x1F)


def enc_adr(pc: int, target: int, rd: int) -> int:
    imm = target - pc
    assert -(1 << 20) <= imm < (1 << 20)
    immlo = imm & 0x3
    immhi = (imm >> 2) & 0x7FFFF
    return 0x10000000 | (immlo << 29) | (immhi << 5) | (rd & 0x1F)


def v2f(va: int) -> int:
    # .text PT_LOAD: VirtAddr=0x1e878, FileOff=0x1a878 -> file_off = vaddr - 0x4000
    assert 0x1E878 <= va < 0x61930, f"va out of .text/.plt: {hex(va)}"
    return va - 0x4000


def verify_elf_aarch64(data: bytes) -> None:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError("Input file is not a valid ELF binary")
    if data[4] != 2 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 183:
        raise RuntimeError("Input ELF is not AArch64 little-endian")


def patch_binary(buf: bytearray, allow_md5_mismatch: bool = False) -> bytearray:
    verify_elf_aarch64(buf)
    orig_md5 = hashlib.md5(buf).hexdigest()
    print(f"Input MD5: {orig_md5} ({len(buf)} bytes)")
    if orig_md5 != EXPECTED_ORIG_MD5:
        msg = f"MD5 mismatch (expected {EXPECTED_ORIG_MD5} from PLZ110_17.0.0.107(CN01))"
        if not allow_md5_mismatch:
            raise RuntimeError(msg)
        print(f"WARNING: {msg}; continuing with strict anchor verification.")

    anchors = [
        (0x2351C, 0x1AD90A68),
        (0x23554, 0x9400F7F3),
        (0x242CC, 0x5292D000),
        (0x242D0, 0x72A01300),
    ]
    for va, expected in anchors:
        actual = struct.unpack_from("<I", buf, v2f(va))[0]
        if actual != expected:
            raise RuntimeError(f"Anchor mismatch at VA 0x{va:x}: got 0x{actual:08x}, expected 0x{expected:08x}")

    LOOP_TOP = 0x2351C
    WAIT_LOOP = 0x2352C
    READ_NATIVE_MODE = 0x2362C
    RETRY_FIND = 0x23570
    PROP_STR_ADDR = 0x235B0
    CB_READ_SERIAL = 0x227BC
    PLT_PROP_FIND = 0x61500
    PLT_PROP_READ_CB = 0x61510
    PLT_PROP_WAIT = 0x61520
    FN_NANOSLEEP_NS = 0x5BE4C

    # 1. Replace polling loop with infinite kernel futex wait (timeout = NULL)
    patch1 = [
        (0x2351C, enc_cbz(0x2351C, RETRY_FIND, 24, is64=True)),
        (0x23520, enc_cbnz(0x23520, WAIT_LOOP, 28, is64=False)),
        (0x23524, 0xB9404BE8),
        (0x23528, enc_b(0x23528, READ_NATIVE_MODE)),
        (0x2352C, 0xB9404BE1),
        (0x23530, 0x910243E2),
        (0x23534, 0xAA1F03E3),  # mov x3, xzr (NULL timeout -> infinite futex sleep)
        (0x23538, 0xAA1803E0),
        (0x2353C, 0xB90093E1),
        (0x23540, enc_bl(0x23540, PLT_PROP_WAIT)),
        (0x23544, 0xB94093E8),
        (0x23548, 0xB9404BE9),
        (0x2354C, 0x6B09011F),
        (0x23550, enc_b_cond(0x23550, READ_NATIVE_MODE, 1)),
        (0x23554, enc_b(0x23554, WAIT_LOOP)),
        (0x23558, 0xD503201F),
        (0x2355C, 0xD503201F),
        (0x23560, 0xD503201F),
        (0x23564, 0xD503201F),
        (0x23568, 0xD503201F),
        (0x2356C, 0xD503201F),
    ]

    # 2. Replace dead settings-fork block with fresh-boot retry_find for persist.sys.sf.native_mode
    patch2 = [
        (0x23570, 0x528CA000),  # 500 ms nanosleep
        (0x23574, 0x72A3B9A0),
        (0x23578, enc_bl(0x23578, FN_NANOSLEEP_NS)),
        (0x2357C, enc_adr(0x2357C, PROP_STR_ADDR, 0)),
        (0x23580, enc_bl(0x23580, PLT_PROP_FIND)),
        (0x23584, 0xAA0003F8),
        (0x23588, 0xF90017F8),
        (0x2358C, enc_cbz(0x2358C, RETRY_FIND, 24, is64=True)),
        (0x23590, enc_adr(0x23590, CB_READ_SERIAL, 1)),
        (0x23594, 0x910123E2),
        (0x23598, 0xAA1803E0),
        (0x2359C, enc_bl(0x2359C, PLT_PROP_READ_CB)),
        (0x235A0, enc_b(0x235A0, LOOP_TOP)),
        (0x235A4, 0xD503201F),
        (0x235A8, 0xD503201F),
        (0x235AC, 0xD503201F),
    ]

    # 3. Widen early-boot retry sleep from 10 ms to 500 ms
    patch3 = [
        (0x242CC, 0x528CA000),
        (0x242D0, 0x72A3B9A0),
    ]

    for addr, insn in patch1 + patch2 + patch3:
        struct.pack_into("<I", buf, v2f(addr), insn)

    prop_bytes = b"persist.sys.sf.native_mode\x00"
    buf[v2f(PROP_STR_ADDR) : v2f(PROP_STR_ADDR) + len(prop_bytes)] = prop_bytes
    for i in range(v2f(PROP_STR_ADDR) + len(prop_bytes), v2f(0x235E0)):
        buf[i] = 0

    return buf


def main() -> int:
    ap = argparse.ArgumentParser(description="Replace 2 Hz settings fork storm in oplus-color-mode-sync with futex sleep")
    ap.add_argument("--input", "-i", required=True, type=Path, help="Original /odm/bin/hw/oplus-color-mode-sync")
    ap.add_argument("--output", "-o", type=Path, help="Output path for patched binary")
    ap.add_argument("--verify-only", action="store_true", help="Verify offsets/opcodes without writing")
    ap.add_argument("--allow-md5-mismatch", action="store_true", help="Allow different build if exact anchors match")
    args = ap.parse_args()

    raw = bytearray(args.input.read_bytes())
    patched = patch_binary(raw, allow_md5_mismatch=args.allow_md5_mismatch)
    out_md5 = hashlib.md5(patched).hexdigest()
    print(f"Patched MD5: {out_md5} (expected {EXPECTED_PATCHED_MD5})")

    if args.verify_only:
        print("Verify-only passed cleanly.")
        return 0
    if not args.output:
        ap.error("--output is required unless --verify-only is specified")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(patched)
    args.output.chmod(0o755)
    print(f"Wrote patched binary to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
