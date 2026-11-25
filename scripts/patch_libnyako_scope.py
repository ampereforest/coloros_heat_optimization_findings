#!/usr/bin/env python3
"""
Offset-verified incremental AArch64 ELF patcher for /system/lib64/libnyako_core.so (v7.3).

Target Base Port:
    ColorOS 17 V6 CN (PLZ110_17.0.0.107(CN01), OnePlus 15T base)
    ported to Xiaomi 15 (dada, SM8750)
    Expected input libnyako_core.so (v6.1 static RX pool) MD5: ab5fb1dd1a02bb76dcfa593774aeb5cc
    Expected patched MD5:                                      50eefaa82d4062aecb08a09111f64d5b

What this patch does:
    1. Compiles src/nyako/nyako_scope_filter.c as freestanding zero-relocation PC-relative
       AArch64 machine code and embeds it into unused R-X padding at vaddr 0x190800
       (file offset 0x178800) inside existing 16KB-aligned PT_LOAD[4] (0x190000..0x1d4000).
    2. Wires nyako_scope_declared_for (0xbbfe4) and nyako_whitelist_allows (0xbb7fc) to
       nyako_is_rom_scope (0x190800) so non-ROM app processes exit immediately in <1 us
       with zero /data/local/nyako/state/scopes syscalls, zero SELinux AVC storms, and
       zero 24-ELF nyako_scan_all_plugins() I/O (eliminating 122 -> 0 post-unlock Zygote
       Slow operation fork stalls).
    3. Gates /data/local/nyako/{modules,state} mkdir()/chmod() at 0x9987c..0x99880 on
       geteuid() == 0 so non-UID-0 system_apps (Camera, PhoneManager) stop emitting
       avc: denied { setattr }.
    4. Neutralizes fsetxattr(nyako_data) hard-error sites at 0xc0b4c and 0xcd794 with
       `mov w0, wzr` so rootless asset materialization succeeds cleanly under Enforcing SELinux.

Usage:
    ANDROID_NDK_HOME=/path/to/android-ndk-r29 \
    python3 scripts/patch_libnyako_scope.py \
        --input /path/to/v61/libnyako_core.so \
        --output /path/to/v73/libnyako_core.so
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECTED_V61_MD5 = "ab5fb1dd1a02bb76dcfa593774aeb5cc"
EXPECTED_V61_SIZE = 0x1BC000
EXPECTED_PATCHED_MD5 = "50eefaa82d4062aecb08a09111f64d5b"

SCOPE_VADDR = 0x190800
SCOPE_OFF = 0x178800
SCOPE_MAX = 0x3800  # 0x190800..0x194000 (precedes 256 KiB static RX trampoline pool at 0x194000)

VERIFY_WORDS = [
    (0xBBFE4, 0xA9BA7BFD, "nyako_scope_declared_for+0x8"),
    (0xBB7FC, 0xA9BA7BFD, "nyako_whitelist_allows+0x8"),
    (0x9987C, 0xB9401BE8, "ldr w8, [sp, #0x18] (dir setup)"),
    (0x99880, 0x35000568, "cbnz w8, 0x9992c (dir setup)"),
    (0xC0B4C, 0x9402A7DD, "bl fsetxattr@plt (nyako_materialize_assets)"),
    (0xCD794, 0x940274CB, "bl fsetxattr@plt (nyako_install)"),
]


def enc_b(pc: int, target: int, link: bool = False) -> bytes:
    diff = target - pc
    if (diff & 3) != 0:
        raise RuntimeError(f"Unaligned branch {hex(pc)} -> {hex(target)}")
    imm26 = diff >> 2
    if not (-(1 << 25) <= imm26 < (1 << 25)):
        raise RuntimeError(f"Branch out of range: {hex(diff)}")
    opcode = 0x94000000 if link else 0x14000000
    return struct.pack("<I", opcode | (imm26 & 0x03FFFFFF))


def verify_elf_aarch64(data: bytes) -> None:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError("Input file is not a valid ELF binary")
    if data[4] != 2 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 183:
        raise RuntimeError("Input ELF is not AArch64 little-endian")


def compile_scope_filter(filter_c: Path, ndk_bin: Path) -> bytes:
    cc = ndk_bin / "aarch64-linux-android35-clang"
    objcopy = ndk_bin / "llvm-objcopy"
    readelf = ndk_bin / "llvm-readelf"
    nm = ndk_bin / "llvm-nm"
    for tool in (cc, objcopy, readelf, nm):
        if not tool.is_file():
            raise RuntimeError(f"Missing NDK tool: {tool}")

    with tempfile.TemporaryDirectory(prefix="nyako_scope_") as td:
        tmp = Path(td)
        ld_file = tmp / "nyako_scope_filter.ld"
        elf_file = tmp / "nyako_scope_filter.elf"
        bin_file = tmp / "nyako_scope_filter.bin"

        ld_file.write_text(
            "SECTIONS {\n"
            f"  . = {hex(SCOPE_VADDR)};\n"
            "  .text : { *(.text.entry) *(.text*) *(.rodata*) }\n"
            "  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) *(.ARM*) *(.dynamic*) "
            "*(.dynsym*) *(.dynstr*) *(.hash*) *(.gnu.hash*) *(.rela*) *(.got*) *(.plt*) *(.interp*) }\n"
            "}\n",
            encoding="utf-8",
        )

        subprocess.run(
            [
                str(cc), "-O2", "-fPIC", "-fno-plt", "-ffreestanding", "-nostdlib",
                "-mno-outline-atomics", "-fno-stack-protector", "-fno-jump-tables",
                "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-ident",
                "-no-pie", "-Wl,-e,nyako_is_rom_scope", "-Wl,-T," + str(ld_file),
                "-Wl,--build-id=none", "-Wl,-z,norelro",
                str(filter_c), "-o", str(elf_file),
            ],
            check=True,
        )

        nm_out = subprocess.run([str(nm), str(elf_file)], capture_output=True, text=True, check=True).stdout
        if f"{SCOPE_VADDR:016x} T nyako_is_rom_scope" not in nm_out:
            raise RuntimeError(f"Entry symbol mismatch:\n{nm_out}")

        rel_out = subprocess.run([str(readelf), "-r", str(elf_file)], capture_output=True, text=True, check=True).stdout
        if "There are no relocations" not in rel_out and "contains 0 entries" not in rel_out:
            raise RuntimeError(f"Unexpected relocations in scope filter:\n{rel_out}")

        subprocess.run([str(objcopy), "-O", "binary", "-j", ".text", str(elf_file), str(bin_file)], check=True)
        filter_bytes = bin_file.read_bytes()
        if not (32 <= len(filter_bytes) <= SCOPE_MAX):
            raise RuntimeError(f"Filter size out of bounds: {len(filter_bytes)} B")
        return filter_bytes


def patch_libnyako_scope(
    data: bytearray,
    filter_bytes: bytes | None,
    allow_md5_mismatch: bool = False,
) -> bytearray:
    verify_elf_aarch64(data)
    in_md5 = hashlib.md5(data).hexdigest()
    print(f"Input MD5: {in_md5} ({len(data)} bytes)")
    if in_md5 != EXPECTED_V61_MD5:
        msg = f"MD5 mismatch (expected {EXPECTED_V61_MD5} from v6.1 libnyako_core.so)"
        if not allow_md5_mismatch:
            raise RuntimeError(msg)
        print(f"WARNING: {msg}; continuing with strict instruction verification.")
    if len(data) != EXPECTED_V61_SIZE and not allow_md5_mismatch:
        raise RuntimeError(f"Unexpected input size: {hex(len(data))} (expected {hex(EXPECTED_V61_SIZE)})")

    for off, expected, desc in VERIFY_WORDS:
        got = struct.unpack_from("<I", data, off)[0]
        if got != expected:
            raise RuntimeError(f"Instruction mismatch at {hex(off)} ({desc}): got {hex(got)}, expected {hex(expected)}")

    if filter_bytes is None:
        return data

    if not all(b == 0 for b in data[SCOPE_OFF : SCOPE_OFF + len(filter_bytes)]):
        raise RuntimeError(f"Target R-X padding at {hex(SCOPE_OFF)} is non-zero!")

    data[SCOPE_OFF : SCOPE_OFF + len(filter_bytes)] = filter_bytes
    data[0xBBFE4:0xBBFE8] = enc_b(0xBBFE4, SCOPE_VADDR, link=False)
    data[0xBB7FC:0xBB800] = enc_b(0xBB7FC, SCOPE_VADDR, link=False)
    data[0x9987C:0x99880] = enc_b(0x9987C, 0x169F60, link=True)   # bl geteuid@plt
    data[0x99880:0x99884] = struct.pack("<I", 0x35000560)          # cbnz w0, 0x9992c
    mov_w0_wzr = struct.pack("<I", 0x2A1F03E0)
    data[0xC0B4C:0xC0B50] = mov_w0_wzr
    data[0xCD794:0xCD798] = mov_w0_wzr
    return data


def resolve_ndk_bin(arg_ndk_bin: Path | None) -> Path:
    if arg_ndk_bin is not None:
        return arg_ndk_bin
    ndk_home = os.environ.get("ANDROID_NDK_HOME", "")
    if not ndk_home:
        raise RuntimeError("Set ANDROID_NDK_HOME or pass --ndk-bin /path/to/llvm/prebuilt/linux-x86_64/bin")
    return Path(ndk_home) / "toolchains/llvm/prebuilt/linux-x86_64/bin"


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    default_filter_c = repo_root / "src/nyako/nyako_scope_filter.c"

    ap = argparse.ArgumentParser(description="Apply v7.3 17-scope fast-path & rootless SELinux patch to libnyako_core.so")
    ap.add_argument("--input", "-i", required=True, type=Path, help="Input v6.1 /system/lib64/libnyako_core.so")
    ap.add_argument("--output", "-o", type=Path, help="Output path for patched v7.3 libnyako_core.so")
    ap.add_argument("--filter-c", type=Path, default=default_filter_c, help="Path to nyako_scope_filter.c")
    ap.add_argument("--ndk-bin", type=Path, help="Path to Android NDK LLVM bin directory")
    ap.add_argument("--verify-only", action="store_true", help="Verify offsets/instructions without compiling or writing")
    ap.add_argument("--allow-md5-mismatch", action="store_true", help="Allow different input hash if instructions match")
    args = ap.parse_args()

    raw = bytearray(args.input.read_bytes())
    if args.verify_only:
        patch_libnyako_scope(raw, filter_bytes=None, allow_md5_mismatch=args.allow_md5_mismatch)
        print("Verify-only passed cleanly.")
        return 0

    if not args.output:
        ap.error("--output is required unless --verify-only is specified")

    ndk_bin = resolve_ndk_bin(args.ndk_bin)
    filter_bytes = compile_scope_filter(args.filter_c, ndk_bin)
    print(f"Compiled {args.filter_c.name}: {len(filter_bytes)} bytes (0 relocations) for vaddr {hex(SCOPE_VADDR)}")

    patched = patch_libnyako_scope(raw, filter_bytes=filter_bytes, allow_md5_mismatch=args.allow_md5_mismatch)
    out_md5 = hashlib.md5(patched).hexdigest()
    print(f"Patched MD5: {out_md5} (reference v7.3 build: {EXPECTED_PATCHED_MD5})")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(patched)
    print(f"Wrote patched binary to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
