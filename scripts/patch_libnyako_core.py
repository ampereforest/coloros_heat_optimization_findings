#!/usr/bin/env python3
"""
Offset-verified Step A AArch64 ELF patcher for /system/lib64/libnyako_core.so (v6.1 zero-execmem).

Eliminates all execmem / memfd_file(execute) calls in Dobby and LSPlant by embedding a
pre-mapped 256 KiB R-X private COW trampoline pool + zero-execmem /proc/self/mem CodePatch
helper (src/nyako/nyako_noexec_stub.c) in a new 16 KiB-aligned R-X PT_LOAD[4] segment
(vaddr 0x190000, file offset 0x178000, size 0x44000).

After Step A (this script), run Step B (scripts/patch_libnyako_scope.py) to add the v7.3
17-scope fast-path filter, UID-0 chmod gate, and fsetxattr no-op (or deploy
prebuilt/system/lib64/libnyako_core.so directly).

Target Base Port:
    ColorOS 17 V6 CN (PLZ110_17.0.0.107(CN01), OnePlus 15T base)
    ported to Xiaomi 15 (dada, SM8750)
    Expected base libnyako_core.so size: 0x1774c0 (1,537,216 bytes)
    Expected base libnyako_core.so MD5:  c63b3763c7949a13fc8d67dfbaf2021f

Usage:
    ANDROID_NDK_HOME=/path/to/android-ndk-r29 \
    python3 scripts/patch_libnyako_core.py \
        --input /path/to/base/libnyako_core.so \
        --output /path/to/v61/libnyako_core.so
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

STUB_VADDR = 0x190000
STUB_OFF = 0x178000
STUB_CODE_MAX = 0x4000
POOL_SIZE = 0x40000
SEG_SIZE = STUB_CODE_MAX + POOL_SIZE  # 0x44000
EXPECTED_SIZE = 0x1774C0

EXPECTED_WORDS = {
    0x105214: 0xA9BC67FE,  # stp x30, x25, [sp, #-0x40]!
    0x1049A8: 0xAA1503E0,  # mov x0, x21
    0x1049AC: 0x2A1F03E1,  # mov w1, wzr
    0x1049B0: 0x94000202,  # bl 0x1051b8
    0x1049BC: 0xAA0003F6,  # mov x22, x0
    0x1049C0: 0x9400020C,  # bl 0x1051f0
    0x0D24AC: 0x7100051F,  # cmp w8, #0x1
    0x0D25BC: 0xF941C101,  # ldr x1, [x8, #0x380]
    0x0D25C0: 0xAA1F03E0,  # mov x0, xzr
    0x0D25D4: 0x94025F27,  # bl mmap@plt
    0x0D2700: 0xB9001128,  # str w8, [x9, #0x10]
    0x0D2704: 0x3D800120,  # str q0, [x9]
    0x0D2708: 0x94024B34,  # bl __clear_cache
}


def enc_b(pc: int, target: int, link: bool = False) -> bytes:
    diff = target - pc
    assert (diff & 3) == 0, f"Unaligned branch {hex(pc)} -> {hex(target)}"
    imm26 = diff >> 2
    assert -(1 << 25) <= imm26 < (1 << 25), f"Branch out of range: {hex(diff)}"
    opcode = 0x94000000 if link else 0x14000000
    return struct.pack("<I", opcode | (imm26 & 0x03FFFFFF))


def find_ndk_bin() -> Path:
    ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("NDK_HOME")
    if not ndk:
        sys.exit("ERROR: Set ANDROID_NDK_HOME to an Android NDK directory (e.g., android-ndk-r29).")
    bin_dir = Path(ndk) / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    if not (bin_dir / "aarch64-linux-android35-clang").exists():
        sys.exit(f"ERROR: clang not found under {bin_dir}")
    return bin_dir


def verify_input(data: bytes) -> None:
    if data[:4] != b"\x7fELF":
        sys.exit("ERROR: Input is not an ELF binary.")
    if len(data) != EXPECTED_SIZE:
        sys.exit(f"ERROR: Unexpected libnyako_core.so size: {hex(len(data))} (expected {hex(EXPECTED_SIZE)}).")
    for off, expected in EXPECTED_WORDS.items():
        got = struct.unpack_from("<I", data, off)[0]
        if got != expected:
            sys.exit(f"ERROR: Mismatch at {hex(off)}: got {hex(got)}, expected {hex(expected)}")
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    if not (e_phoff == 0x40 and e_phentsize == 56 and e_phnum == 9):
        sys.exit(f"ERROR: Unexpected ELF phdr table: off={hex(e_phoff)} size={e_phentsize} num={e_phnum}")
    phdrs = [list(struct.unpack_from("<IIQQQQQQ", data, e_phoff + i * 56)) for i in range(e_phnum)]
    if not (phdrs[3][0] == 1 and phdrs[3][3] == 0x17E678 and phdrs[3][6] == 0xD9F8):
        sys.exit(f"ERROR: Unexpected PT_LOAD[3]: {phdrs[3]}")
    if phdrs[7][0] != 0x6474E551:
        sys.exit(f"ERROR: Expected PT_GNU_STACK at index 7, got {hex(phdrs[7][0])}")


def compile_stub(stub_c: Path, ndk_bin: Path) -> bytes:
    cc = ndk_bin / "aarch64-linux-android35-clang"
    objcopy = ndk_bin / "llvm-objcopy"
    readelf = ndk_bin / "llvm-readelf"
    nm = ndk_bin / "llvm-nm"

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        stub_ld = tdp / "nyako_noexec_stub.ld"
        stub_elf = tdp / "nyako_noexec_stub.elf"
        stub_bin = tdp / "nyako_noexec_stub.bin"
        stub_ld.write_text(
            "SECTIONS {\n"
            f"  . = {hex(STUB_VADDR)};\n"
            "  .text : { *(.text.entry) *(.text*) *(.rodata*) }\n"
            "  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) *(.ARM*) *(.dynamic*) *(.dynsym*) *(.dynstr*) *(.hash*) *(.gnu.hash*) *(.rela*) *(.got*) *(.plt*) *(.interp*) }\n"
            "}\n"
        )
        subprocess.run([
            str(cc), "-O2", "-fPIC", "-fno-plt", "-ffreestanding", "-nostdlib", "-mno-outline-atomics",
            "-fno-stack-protector", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
            "-fno-ident", "-no-pie", "-Wl,-e,_stub_start", "-Wl,-T," + str(stub_ld),
            "-Wl,--build-id=none", "-Wl,-z,norelro",
            str(stub_c), "-o", str(stub_elf),
        ], check=True)

        nm_out = subprocess.run([str(nm), str(stub_elf)], capture_output=True, text=True, check=True).stdout
        assert f"{STUB_VADDR:016x} T _stub_start" in nm_out, f"_stub_start not at {hex(STUB_VADDR)}:\n{nm_out}"
        rel_out = subprocess.run([str(readelf), "-r", str(stub_elf)], capture_output=True, text=True, check=True).stdout
        assert ("There are no relocations" in rel_out or "contains 0 entries" in rel_out), f"Unexpected relocations:\n{rel_out}"

        subprocess.run([str(objcopy), "-O", "binary", "-j", ".text", str(stub_elf), str(stub_bin)], check=True)
        stub_bytes = stub_bin.read_bytes()
        assert 16 < len(stub_bytes) <= STUB_CODE_MAX
        for idx in range(4):
            w = struct.unpack_from("<I", stub_bytes, idx * 4)[0]
            assert (w & 0xFC000000) == 0x14000000, f"Entry {idx} is not unconditional branch: {hex(w)}"
        return stub_bytes


def main() -> int:
    default_stub = Path(__file__).resolve().parent.parent / "src/nyako/nyako_noexec_stub.c"
    ap = argparse.ArgumentParser(description="Zero-execmem PT_LOAD[4] patcher for libnyako_core.so (Step A)")
    ap.add_argument("-i", "--input", required=True, type=Path, help="Base ColorOS 17 V6 libnyako_core.so")
    ap.add_argument("-o", "--output", type=Path, help="Output v6.1 zero-execmem libnyako_core.so")
    ap.add_argument("--stub", default=default_stub, type=Path, help="Path to nyako_noexec_stub.c")
    ap.add_argument("--verify-only", action="store_true", help="Verify input binary offsets/headers and exit")
    args = ap.parse_args()

    raw = args.input.read_bytes()
    verify_input(raw)
    print(f"Input verified: {args.input} ({len(raw):,} bytes, md5={hashlib.md5(raw).hexdigest()})")
    if args.verify_only:
        return 0
    if not args.output:
        sys.exit("ERROR: --output is required unless --verify-only is passed.")

    ndk_bin = find_ndk_bin()
    stub_bytes = compile_stub(args.stub, ndk_bin)
    print(f"Compiled stub: {len(stub_bytes)} bytes (limit {STUB_CODE_MAX}), _stub_start={hex(STUB_VADDR)} VERIFIED")

    data = bytearray(raw)
    nop = struct.pack("<I", 0xD503201F)

    # Patch 1: 0x105214 (Dobby CodePatch) -> b 0x190000
    data[0x105214:0x105218] = enc_b(0x105214, STUB_VADDR + 0x00, link=False)

    # Patch 2: 0x1049ac..0x1049c4 (Dobby NearMemoryArena page alloc)
    data[0x1049AC:0x1049B0] = enc_b(0x1049AC, STUB_VADDR + 0x04, link=True)
    data[0x1049B0:0x1049B4] = struct.pack("<I", 0xAA0003F6)
    data[0x1049B4:0x1049C4] = nop * 4

    # Patch 3a: 0xd24ac -> b 0xd25b8 (skip memfd_create + /dev/ashmem -> zero memfd AVC)
    data[0x0D24AC:0x0D24B0] = enc_b(0x0D24AC, 0x0D25B8, link=False)

    # Patch 3b: 0xd25c0..0xd25d8 (LSPlant page alloc)
    data[0x0D25C0:0x0D25C4] = struct.pack("<I", 0xAA0103E0)
    data[0x0D25C4:0x0D25C8] = enc_b(0x0D25C4, STUB_VADDR + 0x08, link=True)
    data[0x0D25C8:0x0D25D8] = nop * 4

    # Patch 4: 0xd2700..0xd270c (LSPlant 20-byte trampoline write)
    data[0x0D2700:0x0D2704] = struct.pack("<I", 0xD100C3A1)
    data[0x0D2704:0x0D2708] = struct.pack("<I", 0x52800282)
    data[0x0D2708:0x0D270C] = enc_b(0x0D2708, STUB_VADDR + 0x0C, link=True)

    # Rewrite Program Headers: extend PT_LOAD[3] memsz to 0xdc00, replace PT_GNU_STACK with PT_LOAD[4]
    e_phoff = 0x40
    phdrs = [list(struct.unpack_from("<IIQQQQQQ", data, e_phoff + i * 56)) for i in range(9)]
    phdrs[3][6] = 0xDC00
    new_load = [1, 5, STUB_OFF, STUB_VADDR, STUB_VADDR, SEG_SIZE, SEG_SIZE, 0x4000]
    new_phdrs = [
        phdrs[0],  # PT_PHDR
        phdrs[1],  # PT_LOAD 0x000000 R E
        phdrs[2],  # PT_LOAD 0x170080 R W (RELRO)
        phdrs[3],  # PT_LOAD 0x17e678 R W (.data + .bss + 0x18c100 cursor)
        new_load,  # PT_LOAD 0x190000 R E (stub + 256 KiB trampoline pool)
        phdrs[4],  # PT_DYNAMIC
        phdrs[5],  # PT_GNU_RELRO
        phdrs[6],  # PT_GNU_EH_FRAME
        phdrs[8],  # PT_NOTE
    ]
    for i, ph in enumerate(new_phdrs):
        struct.pack_into("<IIQQQQQQ", data, e_phoff + i * 56, *ph)

    assert len(data) <= STUB_OFF
    data.extend(b"\x00" * (STUB_OFF - len(data)))
    data.extend(stub_bytes)
    data.extend(b"\x00" * (SEG_SIZE - len(stub_bytes)))
    assert len(data) == STUB_OFF + SEG_SIZE

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"Wrote {args.output}: {len(data):,} bytes, md5={hashlib.md5(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
