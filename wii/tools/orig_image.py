#!/usr/bin/env python3
"""The game as the decomp's MWCC build links it, for the Wii: its DOL's sections
at their addresses, with the SDK's hardware layers replaced by the port's.

    orig_image.py DOL SYMBOLS SPLITS LDSCRIPTS OUT [--keep UNIT_PREFIX ...]
                  [--port ARCHIVE_OR_OBJECT ...] [--nm NM]

DOL is the decomp's build/GALE01/main.dol (byte for byte the GameCube's, built
from the sources), SYMBOLS and SPLITS its config/GALE01 files, LDSCRIPTS
devkitPPC's (rvl.ld and libogc_common.ld). Writes into OUT:

- image.S: the DOL's sections (incbin'd from dolN.bin), as sections .melee.N;
- tramp.S: melee_tramp_table, {original address, port function} for every
  global function of the SDK's units that are not kept, which the port's
  main() patches to branch to the port's (the port's own function of that
  name, from the --port archives; a stub that reports the missing function if
  there is none);
- melee.ld: the linker script. The DOL's sections where they were; then the
  port (libogc_common.ld) from the end of the DOL's (0x804DEC00, where its
  stack was), its small data first: r13 and r2 are the GameCube's
  (_SDA_BASE_ 0x804DB6A0, _SDA2_BASE_ 0x804DF9E0), which the game's code
  addresses its small data from, and the port's has to be in their reach too.
  Every global symbol of the units kept (the game's, the SDK's that are kept)
  is PROVIDEd at its address, for the port to call.

Why: this is the GameCube's game, instruction for instruction, so it computes
the same frames as the GameCube (Slippi's online play runs a Wii and Dolphin in
lockstep), and its code and data are where Slippi's codes (Gecko codes, for the
GameCube's DOL) patch and call them. The kept SDK units (--keep: GX, the
matrix library...) run on the Wii as they are; the others' entry points branch
to the port.
"""

import argparse
import re
import struct
import subprocess
from pathlib import Path

# The end of what Slippi's codes use past the DOL's sections (above)
SLIPPI_RESERVED_END = 0x804E2800

# The GameCube's r13 and r2 (an --elf has its own)
SDA_BASE = 0x804DB6A0
SDA2_BASE = 0x804DF9E0
PORT_MEM2 = 0x90002000  # the rest of the port (libogc's MEM2 arena starts after it)
PORT_MEM2_END = 0x90800000
PORT_LOAD = 0x81000000  # where the MEM2 part is loaded, in the game's arena to be
PORT_LOAD_END = 0x81700000
# The SDK's data the DSP reads (.melee_dsp), at the top of MEM1 (copied there
# at the start, melee_boot), and libogc's own MEM1 under it (its kernel
# workspace, 64 KiB, and room to spare): the game's arena is the GameCube's,
# from the end of its main stack, as mods like 20XX (static branches into
# the files it loads) need
DSP_RESERVE = 0x1A000
OGC_ARENA1 = 0x14000
# The GameCube's arena start (the end of the main stack: _stack_addr, where
# OSInit puts it with no debugger; an --elf has its own)
ARENA_LO = 0x804EEC00

UNIT = re.compile(r"^(\S.*):\s*$")
RANGE = re.compile(r"^\s+(\S+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)")
SYMBOL = re.compile(r"^(\S+) = (\S+):0x([0-9A-Fa-f]+); // type:(\w+)(.*)$")


def read_dol(path: Path):
    data = path.read_bytes()
    offs = struct.unpack(">18I", data[0:72])
    addrs = struct.unpack(">18I", data[72:144])
    sizes = struct.unpack(">18I", data[144:216])
    bss_addr, bss_size, entry = struct.unpack(">3I", data[216:228])
    sections = []
    for i in range(18):
        if sizes[i]:
            sections.append((i, i < 7, addrs[i], data[offs[i]:offs[i] + sizes[i]]))
    return sections, bss_addr, bss_size, entry


def read_splits(path: Path):
    """[(unit, section, start, end)]"""
    out = []
    unit = None
    for line in path.read_text().splitlines():
        m = UNIT.match(line)
        if m and m.group(1) != "Sections":
            unit = m.group(1)
            continue
        m = RANGE.match(line)
        if m and unit is not None:
            out.append((unit, m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return out


def read_symbols(path: Path):
    """[(name, section, address, type, scope)]"""
    out = []
    for line in path.read_text().splitlines():
        m = SYMBOL.match(line)
        if not m:
            continue
        rest = m.group(5)
        scope = re.search(r"scope:(\w+)", rest)
        out.append((m.group(1), m.group(2), int(m.group(3), 16), m.group(4),
                    scope.group(1) if scope else "global"))
    return out


def defined(nm: str, paths: list[str]) -> set[str]:
    out = subprocess.run([nm, "-g", "--defined-only", *paths], capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TW":
            names.add(parts[2])
    return names


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dol", type=Path)
    ap.add_argument("symbols", type=Path)
    ap.add_argument("splits", type=Path)
    ap.add_argument("ldscripts", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--keep", action="append", default=[],
                    help="SDK units (prefixes of their names in splits.txt) that stay as they are")
    ap.add_argument("--port", action="append", default=[])
    ap.add_argument("--override", action="append", default=[],
                    help="a function of a kept unit that branches to the port's all the same")
    ap.add_argument("--nm", default="powerpc-eabi-nm")
    ap.add_argument("--elf", type=Path,
                    help="the DOL's ELF (build/GALE01/main.elf): its symbols' addresses, where the DOL is "
                         "not the GameCube's own (a test build with code added); SYMBOLS and SPLITS still "
                         "say which unit a symbol is of")
    ap.add_argument("--redirect", action="append", default=[],
                    help="CALLER:CALLEE:TARGET: the calls CALLER makes to CALLEE go to the port's TARGET")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    sections, bss_addr, bss_size, _ = read_dol(args.dol)
    splits = read_splits(args.splits)
    symbols = read_symbols(args.symbols)
    sizes = {}
    for line in args.symbols.read_text().splitlines():
        m = SYMBOL.match(line)
        if m:
            size = re.search(r"size:0x([0-9A-Fa-f]+)", m.group(5))
            if size:
                sizes[int(m.group(3), 16)] = int(size.group(1), 16)

    # Where each symbol is in this DOL: its address in SYMBOLS, or in --elf
    elf = {}
    if args.elf:
        out = subprocess.run([args.nm, "-S", "--defined-only", str(args.elf)], capture_output=True,
                             text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 4:
                elf.setdefault(parts[3], []).append((int(parts[0], 16), int(parts[1], 16)))
            elif len(parts) == 3:  # (no size: an absolute symbol, _SDA_BASE_)
                elf.setdefault(parts[2], []).append((int(parts[0], 16), 0))

    def moved(name: str, address: int) -> int:
        found = elf.get(name)
        if found and len(found) == 1:
            return found[0][0]
        return address

    def size_of(name: str, address: int) -> int:
        found = elf.get(name)
        if found and len(found) == 1:
            return found[0][1]
        return sizes.get(address, 0)

    def unit_of(address: int, section: str):
        for unit, sec, start, end in splits:
            if sec == section and start <= address < end:
                return unit
        return None

    def replaced(unit) -> bool:
        if unit is None:
            return False
        if not unit.startswith("dolphin/") and not unit.startswith("MetroTRK/"):
            return False
        return not any(unit.startswith(k) for k in args.keep)

    # image.S
    # The sections below 0x80003F00 overlap the Wii's own globals there
    # (0x80003100 on: the memory's sizes, IOS's version, the IPC buffer...),
    # which libogc reads as it starts: they are not loaded where they go, but
    # carried in the port's data (melee_late_table: {address, size, bytes}),
    # and copied there by the port's main() once libogc has started
    late = [(i, addr, data) for i, is_text, addr, data in sections if addr < 0x80003F00]
    sections = [x for x in sections if x[2] >= 0x80003F00]
    image = ["/* Written by wii/tools/orig_image.py: the decomp's DOL, section by section */"]
    for i, is_text, addr, data in sections:
        (args.out / f"dol{i}.bin").write_bytes(data)
        flags = "ax" if is_text else "aw"
        image.append(f'\t.section .melee.{i},"{flags}",@progbits')
        image.append(f'\t.incbin "{(args.out / f"dol{i}.bin").resolve()}"')
    image.append('\t.section .rodata.melee_late,"a"')
    image.append("\t.balign 32")
    image.append("\t.globl melee_late_table")
    image.append("melee_late_table:")
    for i, addr, data in late:
        image.append(f"\t.long {addr:#x}, {len(data):#x}, melee_late_{i}")
    image.append("\t.long 0, 0, 0")
    for i, addr, data in late:
        (args.out / f"dol{i}.bin").write_bytes(data)
        image.append("\t.balign 32")
        image.append(f"melee_late_{i}:")
        image.append(f'\t.incbin "{(args.out / f"dol{i}.bin").resolve()}"')
    (args.out / "image.S").write_text("\n".join(image) + "\n")

    # tramp.S: the replaced SDK functions
    available = defined(args.nm, args.port) if args.port else set()
    tramps = []
    missing = []
    provides = []
    for name, section, address, kind, scope in symbols:
        unit = unit_of(address, section)
        if name in args.override:
            if name not in available:
                raise SystemExit(f"orig_image: the port has no {name} to override the game's")
            tramps.append((moved(name, address), name, size_of(name, address)))
            continue
        if replaced(unit):
            if kind == "function" and scope in ("global", "weak") and section in (".text", ".init"):
                if name in available:
                    tramps.append((moved(name, address), name, size_of(name, address)))
                elif not unit.startswith("MetroTRK/"):
                    # (MetroTRK's code is the island: nothing calls it)
                    missing.append((moved(name, address), name))
            continue
        if scope in ("global", "weak") and name != "main" and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
            provides.append((name, moved(name, address)))
    t = ["/* Written by wii/tools/orig_image.py: the SDK's functions the port replaces */",
         '\t.section .rodata.melee_tramp,"a"',
         "\t.balign 4",
         "\t.globl melee_tramp_table",
         "melee_tramp_table:"]
    # {site, target, the function's size}: with 16 bytes or more, the jump
    # is written in full at the site (position independent instruction by
    # instruction, so a code that moves the first one, as Gecko hooks do,
    # still jumps); else a branch to a veneer
    for address, name, size in tramps:
        t.append(f"\t.long {address:#x}, {name}, {size:#x}")
    for k, (address, name) in enumerate(missing):
        t.append(f"\t.long {address:#x}, melee_missing_{k}, 0")
    t.append("\t.long 0, 0, 0")

    # Calls redirected (melee_call_table: {call site, port function}): the
    # bl instructions in CALLER's code that reach CALLEE
    by_name = {}
    for name, section, address, kind, scope in symbols:
        by_name.setdefault(name, []).append((moved(name, address), size_of(name, address)))
    t.append("\t.globl melee_call_table")
    t.append("melee_call_table:")
    for spec in args.redirect:
        caller, callee, target = spec.split(":")
        (c_addr, c_size), = by_name[caller]
        (e_addr, _), = by_name[callee]
        found = 0
        for i, is_text, addr, data in sections:
            if not is_text or not addr <= c_addr < addr + len(data):
                continue
            for pc in range(c_addr, c_addr + c_size, 4):
                word = struct.unpack_from(">I", data, pc - addr)[0]
                if word & 0xFC000003 == 0x48000001:
                    disp = word & 0x03FFFFFC
                    if disp & 0x02000000:
                        disp -= 0x04000000
                    if pc + disp == e_addr:
                        t.append(f"\t.long {pc:#x}, {target}")
                        found += 1
        if not found:
            raise SystemExit(f"orig_image: {caller} makes no call to {callee}")
    t.append("\t.long 0, 0")

    # The code the port replaces (melee_replaced_table: {start, end}): the
    # functions of the units that are not kept. A Gecko code that patches
    # them (written for the GameCube's SDK) would patch code that never runs,
    # or the branch to the port's: the port's code applier skips those
    replaced_ranges = []
    for name, section, address, kind, scope in symbols:
        if kind != "function" or section not in (".text", ".init"):
            continue
        if replaced(unit_of(address, section)) or name in args.override:
            start = moved(name, address)
            replaced_ranges.append((start, start + max(size_of(name, address), 4)))
    replaced_ranges.sort()
    merged = []
    for a, b in replaced_ranges:
        if merged and a <= merged[-1][1] + 32:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    t.append("\t.globl melee_replaced_table")
    t.append("melee_replaced_table:")
    for a, b in merged:
        t.append(f"\t.long {a:#x}, {b:#x}")
    t.append("\t.long 0, 0")

    # The zero-initialised ranges (melee_zero_table: {start, end}): the DOL's
    # bss, less the sections loaded inside it (.sdata)
    zero = [(bss_addr, bss_addr + bss_size)]
    for i, is_text, addr, data in sections:
        out = []
        for a, b in zero:
            if addr + len(data) <= a or b <= addr:
                out.append((a, b))
                continue
            if a < addr:
                out.append((a, addr))
            if addr + len(data) < b:
                out.append((addr + len(data), b))
        zero = out
    t.append("\t.globl melee_zero_table")
    t.append("melee_zero_table:")
    dol_end_z = (max(addr + len(data) for _, _, addr, data in sections) + 0x1F) & ~0x1F
    if dol_end_z < SLIPPI_RESERVED_END:
        zero.append((dol_end_z, SLIPPI_RESERVED_END))
    for a, b in zero:
        t.append(f"\t.long {a:#x}, {b:#x}")
    t.append("\t.long 0, 0")
    t.append('\t.section .rodata.melee_missing_names,"a"')
    for k, (address, name) in enumerate(missing):
        t.append(f'melee_missing_name_{k}: .asciz "{name}"')
    t.append('\t.section .text.melee_missing,"ax",@progbits')
    t.append("\t.balign 4")
    for k, (address, name) in enumerate(missing):
        t.append(f"melee_missing_{k}:")
        t.append(f"\tlis 3, melee_missing_name_{k}@ha")
        t.append(f"\taddi 3, 3, melee_missing_name_{k}@l")
        t.append("\tb melee_wii_sdk_missing")
    (args.out / "tramp.S").write_text("\n".join(t) + "\n")

    # melee.ld
    sda_base, sda2_base = SDA_BASE, SDA2_BASE
    if "_SDA_BASE_" in elf:
        sda_base = elf["_SDA_BASE_"][0][0]
    if "_SDA2_BASE_" in elf:
        sda2_base = elf["_SDA2_BASE_"][0][0]
    # The port's MEM1 part starts where the DOL's sections end, past what
    # Slippi's codes keep there: the bottom of the GameCube's main stack
    # (0x804DEC00 on), which it never reaches, holds their static data (r2 and
    # r13 relative, up to 0x804E1F74 in the online codeset); it is zeroed at
    # the start (melee_zero_table), as the GameCube's memory is
    dol_end = (max(addr + len(data) for _, _, addr, data in sections) + 0x1F) & ~0x1F
    port_start = max(dol_end, SLIPPI_RESERVED_END)
    if port_start >= sda_base + 0x8000:
        raise SystemExit("orig_image: the DOL leaves no room in r13's reach")
    # The island (the port's code the game's must reach with a branch: the
    # veneers to the port's code in MEM2, Gecko codes' blocks): MetroTRK's
    # code, which nothing calls (and mods leave alone: 20XX's code goes in
    # Tournament mode's, MCC's and THPDec's)
    trk = [(moved(n, a), size_of(n, a)) for n, sec, a, k, sc in symbols
           if k == "function" and sec == ".text" and (unit_of(a, sec) or "").startswith("MetroTRK/")]
    island_start, island_end = min(a for a, _ in trk), max(a + size for a, size in trk)

    w = []
    w.append("/* Written by wii/tools/orig_image.py: the decomp's DOL where the GameCube has it, then the port */")
    w.append("")
    w.append("/* The port: its small data and its start in MEM1 (r13's and r2's reach, from the end of")
    w.append("   the DOL's sections), the rest in MEM2, so the game's arena in MEM1 is the GameCube's.")
    w.append("   What runs in MEM2 is loaded at PORT_LOAD (in MEM1, which the game's arena takes over")
    w.append("   later), and copied up by melee_boot (wii/src/boot_mem2.S) before libogc starts: a")
    w.append("   loader needs nothing but MEM1. */")
    arena_lo = elf["_stack_addr"][0][0] if "_stack_addr" in elf else ARENA_LO
    w.append("MEMORY {")
    w.append(f"\tmem1 : ORIGIN = {port_start:#x}, LENGTH = {arena_lo - port_start:#x}")
    w.append(f"\tmem1top : ORIGIN = {0x81800000 - DSP_RESERVE:#x}, LENGTH = {DSP_RESERVE:#x}")
    w.append(f"\tmem2 : ORIGIN = {PORT_MEM2:#x}, LENGTH = {PORT_MEM2_END - PORT_MEM2:#x}")
    w.append(f"\tload : ORIGIN = {PORT_LOAD:#x}, LENGTH = {PORT_LOAD_END - PORT_LOAD:#x}")
    w.append("}")
    w.append("")
    # (the port's segments first: elf2dol makes the DOL's sections in this
    # order, and Dolphin takes a DOL for a Wii program by the Wii's own
    # instructions in its first text section)
    w.append("PHDRS {")
    w.append("\ttext PT_LOAD FLAGS(5);")
    w.append("\tboot PT_LOAD FLAGS(5);")
    w.append("\tsmall PT_LOAD FLAGS(6);")
    w.append("\tdsp PT_LOAD FLAGS(6);")
    w.append("\tdata PT_LOAD FLAGS(6);")
    for i, is_text, addr, data in sections:
        w.append(f"\tdol{i} PT_LOAD FLAGS({5 if is_text else 6});")
    w.append("\tbss PT_LOAD;")
    w.append("}")
    w.append("")
    w.append("OUTPUT_ARCH(powerpc:common)")
    w.append("ENTRY(melee_boot)")
    w.append("PROVIDE_HIDDEN( __isIPL = 0 );")
    w.append("PROVIDE_HIDDEN( __ppc_excpt_buf   = 0x800000d0 );")
    w.append("PROVIDE_HIDDEN( __ppc_excpt_table = 0x80003000 );")
    w.append(f"_SDA_BASE_ = {sda_base:#x};")
    w.append(f"_SDA2_BASE_ = {sda2_base:#x};")
    (main_addr, _), = [(moved(n, a), sec) for n, sec, a, k, sc in symbols if n == "main"]
    w.append(f"melee_main = {main_addr:#x};")
    # .ctors: the DOL's third data section (after extab and extabindex), as
    # the ELF's section headers have it where there is one
    ctors = [(0x803B7240, 0x20)]
    if args.elf:
        objdump = args.nm.replace("nm", "objdump") if args.nm.endswith("nm") else "objdump"
        out = subprocess.run([objdump, "-h", str(args.elf)], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[1] == ".ctors":
                ctors = [(int(parts[3], 16), int(parts[2], 16))]
    for addr, size in ctors:
        w.append(f"__melee_ctors_start = {addr:#x};")
        w.append(f"__melee_ctors_end = {addr + size:#x};")
    w.append(f"__melee_image_start = {min([a for _, _, a, _ in sections] + [a for _, a, _ in late]):#x};")
    w.append(f"__melee_image_end = {port_start:#x};")
    w.append(f"__melee_island_start = {island_start:#x};")
    w.append(f"__melee_island_end = {island_end:#x};")
    w.append(f"__melee_arena_lo = {arena_lo:#x};")
    w.append("")
    w.append("SECTIONS {")
    for i, is_text, addr, data in sections:
        w.append(f"\t.melee.{i} {addr:#x} : {{ KEEP(*(.melee.{i})) }} :dol{i}")
    w.append("")
    # The port's MEM1 part: its small data, in r13's and r2's reach (.sbss is
    # written out as zeros: libogc's crt0 clears __sbss_start to __bss_end,
    # which is the port's .bss alone), and the start that moves the rest up
    w.append("\t.sdata2 : { *(.sdata2 .sdata2.* .gnu.linkonce.s2.*) *(.sbss2 .sbss2.* .gnu.linkonce.sb2.*) } > mem1 :small = 0")
    w.append("\t.sdata : { *(.sdata .sdata.* .gnu.linkonce.s.*) *(.lit8) *(.lit4) } > mem1 :small = 0")
    w.append("\t.sbss : { *(.dynsbss) *(.sbss .sbss.* .gnu.linkonce.sb.*) *(.scommon) LONG(0) . = ALIGN(32); } > mem1 :small = 0")
    w.append(f"\tASSERT(ADDR(.sbss) + SIZEOF(.sbss) <= {sda_base + 0x8000:#x}, \"the port's small data is out of r13's reach\")")
    w.append("\t.melee_boot : { KEEP(*(.melee_boot)) . = ALIGN(32); } > mem1 :boot = 0")
    # The SDK's data the DSP reads (AX's parameter blocks, command lists and
    # buffers, its microcode): in MEM1, which the GameCube's microcode
    # addresses (in MEM2 its addresses come out wrong), written out (with its
    # zeros: crt0 clears only the port's .bss)
    # And libogc's SRAM buffer: libogc reads the SRAM into it by EXI DMA at its
    # start, and EXI DMA reaches MEM1 only (in MEM2, the same low address in
    # MEM1 is written: the game's code, there by then)
    w.append("\t.melee_dsp : { *libmelee_sdk.a:*(.data .data.* .rodata .rodata.* .bss .bss.* COMMON) "
             ". = ALIGN(32); *libogc.a:system.o(.bss.sramcntrl) "
             "LONG(0) . = ALIGN(32); } > mem1top AT> load :dsp = 0")
    w.append("\t__dsp_vma_start = ADDR(.melee_dsp);")
    w.append("\t__dsp_load_start = LOADADDR(.melee_dsp);")
    w.append("\t__dsp_load_end = LOADADDR(.melee_dsp) + SIZEOF(.melee_dsp);")
    w.append(f"\tPROVIDE_HIDDEN( __Arena1Lo = ADDR(.melee_dsp) - {OGC_ARENA1:#x} );")
    w.append("\tPROVIDE_HIDDEN( __Arena1Hi = ADDR(.melee_dsp) );")
    w.append("}")
    w.append("")
    for name, address in provides:
        w.append(f"PROVIDE( {name} = {address:#x} );")
    w.append("")
    w.append("PROVIDE_HIDDEN( __Arena2Hi    = 0x933e0000 );")
    w.append("PROVIDE_HIDDEN( __ipcbufferLo = 0x933e0000 );")
    w.append("PROVIDE_HIDDEN( __ipcbufferHi = 0x93400000 );")
    w.append("")

    common = (args.ldscripts / "libogc_common.ld").read_text()
    common = re.sub(r"OUTPUT_ARCH\([^)]*\)\s*", "", common)
    common = re.sub(r"ENTRY\([^)]*\)\s*", "", common)
    common = re.sub(r"PROVIDE_HIDDEN\( __isIPL[^;]*;\s*", "", common)
    common = re.sub(r"PROVIDE_HIDDEN\( __ppc_excpt_\w+\s*=[^;]*;\s*", "", common)
    common = re.sub(r"PHDRS \{[^}]*\}\s*", "", common)
    common = re.sub(r"\tPROVIDE_HIDDEN\( __Arena1(Lo|Hi)[^\n]*\n", "", common)
    for name in (".sdata2", ".sdata", ".sbss"):
        common, n = re.subn(r"\n\t(/\*[^*]*\*/\n\t)?" + re.escape(name) + r" [^{]*\{[^}]*\}[^\n]*\n", "\n",
                            common, count=1)
        if n != 1:
            raise SystemExit(f"orig_image: no {name} in libogc_common.ld")
    common = re.sub(r"\tPROVIDE_HIDDEN\( __sbss_start[^\n]*\n\tPROVIDE_HIDDEN\( __sbss_end[^\n]*\n", "", common)
    old = "PROVIDE_HIDDEN( __bss_end   = ADDR(.bss) + SIZEOF(.bss) );"
    if old not in common:
        raise SystemExit("orig_image: libogc_common.ld's __bss_end not found")
    common = common.replace(old, old + "\n\t__sbss_start = ADDR(.bss);\n\t__sbss_end = ADDR(.bss);"
                            "\n\tPROVIDE_HIDDEN( __Arena2Lo = (ADDR(.bss) + SIZEOF(.bss) + 0x1f) & ~0x1f );", 1)
    # The rest of the port in MEM2, loaded in MEM1
    common, n = re.subn(r"\} :(text|data) = 0", r"} > mem2 AT> load :\1 = 0", common)
    if n < 5:
        raise SystemExit("orig_image: libogc_common.ld's sections not found")
    common, n = re.subn(r"\} :bss", r"} > mem2 :bss", common)
    if n != 1:
        raise SystemExit("orig_image: libogc_common.ld's .bss not found")
    common = common.replace(old, old + "\n\t__port_vma_start = ADDR(.init);"
                            "\n\t__port_load_start = LOADADDR(.init);"
                            "\n\t__port_load_end = LOADADDR(.data) + SIZEOF(.data);", 1)
    w.append(common)
    (args.out / "melee.ld").write_text("\n".join(w) + "\n")
    print(f"orig_image: {len(sections)} sections, {len(tramps)} SDK functions to the port, "
          f"{len(missing)} without one, {len(provides)} symbols provided")
    if missing:
        (args.out / "missing.txt").write_text("".join(f"{a:#x} {n}\n" for a, n in missing))


if __name__ == "__main__":
    main()
