#!/usr/bin/env python3
"""Writes the linker script that lays the game's data out as the GameCube's DOL has it.

    data_layout.py SYMBOLS SPLITS LDSCRIPTS OUT [--prefix DIR ...]

The script is devkitPPC's rvl.ld (and the libogc_common.ld it includes, from
LDSCRIPTS) with the game's sections added; cmake/Wii.cmake has GCC use it
instead of rvl.ld (melee.specs).

The game's sources lean on where MWCC and its linker put their data. Objects
declared one after another are used as one block (toy.c writes past
_Toy_804A26B8 into the objects after it), and buffers the hardware reads or
writes by 32-byte blocks are 32-byte aligned only because of where they fell.
GCC orders and aligns its data otherwise.

So every data object of the game's own sources (the --prefix directories of
splits.txt: melee/ and sysdolphin/) that symbols.txt names is put, by the
section GCC gives it under -fdata-sections (.data.NAME, .bss.NAME, ...), at its
offset in its original section: .data, .rodata, .sdata and .sdata2 into
sections of the loaded image before .sdata (whose small data r13 reaches),
.bss and .sbss into NOLOAD ones
after .bss that main() clears (__melee_bss_start, __melee_bss_end). Each such section starts on 32
bytes, as the original ones did, so an object's address is the same modulo 32
as on the GameCube. What symbols.txt does not name (string literals, MWCC's
constants, GCC's own) goes where the default script puts it.
"""

import argparse
import re
import subprocess
from pathlib import Path

LOADED = [".rodata", ".data", ".sdata", ".sdata2"]
ZEROED = [".bss", ".sbss"]
SYMBOL = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
IDENT = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*$")


def read_splits(path: Path, prefixes: list[str]):
    """{section: [(start, end, basename)]} for the files under the prefixes"""
    ranges: dict[str, list[tuple[int, int, str]]] = {}
    current = None
    for line in path.read_text().splitlines():
        if line and not line[0].isspace() and line.endswith(":"):
            name = line[:-1]
            current = name if any(name.startswith(p) for p in prefixes) else None
            continue
        m = re.match(r"\s+(\.\w+)\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
        if m and current is not None:
            ranges.setdefault(m.group(1), []).append(
                (int(m.group(2), 16), int(m.group(3), 16), Path(current).with_suffix(".o").name))
    return ranges


def section_starts(path: Path):
    """{section: (start, end)} of the original DOL's data sections, from every file's ranges"""
    spans: dict[str, list[int]] = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\s+(\.\w+)\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
        if m:
            s, e = int(m.group(2), 16), int(m.group(3), 16)
            span = spans.setdefault(m.group(1), [s, e])
            span[0] = min(span[0], s)
            span[1] = max(span[1], e)
    return spans


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("symbols", type=Path)
    parser.add_argument("splits", type=Path)
    parser.add_argument("ldscripts", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--prefix", action="append", default=[])
    parser.add_argument("--archive", default="libmelee_game.a")
    parser.add_argument("--readelf", default="powerpc-eabi-readelf")
    parser.add_argument("--objects", type=Path,
                        help="the built game archive: its objects that symbols.txt does not name, but whose "
                             "names end in their address (lbl_80433C64, gm_803DA788.0), are placed there too")
    args = parser.parse_args()

    ranges = read_splits(args.splits, args.prefix or ["melee/", "sysdolphin/"])
    spans = section_starts(args.splits)
    placed: dict[str, list[tuple[int, str, str]]] = {}
    for line in args.symbols.read_text().splitlines():
        m = SYMBOL.match(line)
        if not m:
            continue
        name, section, addr, kind = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        if kind != "object" or section not in LOADED + ZEROED or not IDENT.match(name):
            continue
        for start, end, obj in ranges.get(section, []):
            if start <= addr < end:
                placed.setdefault(section, []).append((addr, name, obj))
                break

    # The rest of the game's data (strings, GCC's constants, what symbols.txt
    # does not name) after what is placed: devkitPPC's own rules leave the
    # game's archive to these
    rest = {".rodata": ".rodata .rodata.* .sdata2 .sdata2.*", ".data": ".data .data.* .sdata .sdata.*",
            ".bss": ".bss .bss.* .sbss .sbss.*"}

    if args.objects is not None:
        known = {name for entries in placed.values() for _, name, _ in entries}
        listing = subprocess.run([args.readelf, "-sW", str(args.objects)], capture_output=True, text=True,
                                 check=True).stdout
        member = None
        added = 0
        for line in listing.splitlines():
            m = re.match(r"^File: .*\((.*)\)$", line)
            if m:
                member = m.group(1)
                continue
            fields = line.split()
            if len(fields) < 8 or fields[3] != "OBJECT" or fields[6] in ("UND", "ABS", "COM"):
                continue
            full = fields[7]
            name = full.split(".")[0]
            m = re.search(r"(8[0-9A-Fa-f]{7})$", name)
            if not m or name in known:
                continue
            addr = int(m.group(1), 16)
            for section in LOADED + ZEROED:
                for start, end, obj in ranges.get(section, []):
                    if start <= addr < end and obj == member:
                        placed.setdefault(section, []).append((addr, name, obj))
                        known.add(name)
                        added += 1
        print(f"data_layout: {added} more by the address in their name")

    def body(section: str, kinds: list[str]) -> list[str]:
        lines = []
        base = spans[section][0]
        for addr, name, obj in sorted(placed.get(section, [])):
            # (a static inside a function is NAME.N to GCC)
            patterns = " ".join(f"{k}.{name} {k}.{name}.*" for k in kinds)
            lines.append(f"\t\t. = MAX(., 0x{addr - base:X});")
            # (kept: devkitPPC links with --gc-sections, and the sources reach
            # some objects only through their neighbours)
            lines.append(f"\t\tKEEP(*{args.archive}:{obj}({patterns}))")
        lines.append(f"\t\t. = MAX(., 0x{spans[section][1] - base:X});")
        if section in rest:
            lines.append(f"\t\t*{args.archive}:*({rest[section]})")
        lines.append("\t\t. = ALIGN(32);")
        return lines

    loaded_kinds = [".rodata", ".data", ".sdata", ".sdata2", ".data.rel.ro", ".data.rel.ro.local", ".bss", ".sbss"]
    loaded = ["\t/* The game's data, as the DOL lays it out (wii/tools/data_layout.py) */"]
    for section in LOADED:
        loaded.append(f"\t.melee{section} ALIGN(32) : {{")
        loaded += body(section, loaded_kinds)
        loaded.append("\t} :data = 0")
    zeroed = ["\t/* The game's zero-initialized data, as the DOL lays it out: main() clears it */",
              "\t. = ALIGN(32);", "\t__melee_bss_start = .;"]
    for section in ZEROED:
        zeroed.append(f"\t.melee{section} ALIGN(32) (NOLOAD) : {{")
        zeroed += body(section, [".bss", ".sbss"])
        zeroed.append("\t} :bss")
    zeroed += ["\t. = ALIGN(32);", "\t__melee_bss_end = .;"]

    common = (args.ldscripts / "libogc_common.ld").read_text().split("\n")
    out_common = []
    exclude = f"EXCLUDE_FILE(*{args.archive}:*)"
    generic = {
        "\t\t*(.rodata .rodata.* .gnu.linkonce.r.*)":
            f"\t\t*({exclude} .rodata {exclude} .rodata.* .gnu.linkonce.r.*)",
        "\t\t*(.sdata2 .sdata2.* .gnu.linkonce.s2.*)":
            f"\t\t*({exclude} .sdata2 {exclude} .sdata2.* .gnu.linkonce.s2.*)",
        "\t\t*(.data .data.* .gnu.linkonce.d.*)":
            f"\t\t*({exclude} .data {exclude} .data.* .gnu.linkonce.d.*)",
        "\t\t*(.sdata .sdata.* .gnu.linkonce.s.*)":
            f"\t\t*({exclude} .sdata {exclude} .sdata.* .gnu.linkonce.s.*)",
        "\t\t*(.sbss .sbss.* .gnu.linkonce.sb.*)":
            f"\t\t*({exclude} .sbss {exclude} .sbss.* .gnu.linkonce.sb.*)",
        "\t\t*(.bss .bss.* .gnu.linkonce.b.*)":
            f"\t\t*({exclude} .bss {exclude} .bss.* .gnu.linkonce.b.*)",
    }
    replaced = 0
    for i, line in enumerate(common):
        if line in generic:
            common[i] = generic[line]
            replaced += 1
    if replaced != len(generic):
        raise SystemExit("data_layout: libogc_common.ld's data rules are not as expected")
    for line in common:
        if line.startswith("\t.sdata : "):
            out_common += loaded + [""]
        if line.startswith("\tPROVIDE_HIDDEN( __Arena1Lo"):
            out_common += zeroed + [""]
        out_common.append(line)
    if len(out_common) != len(common) + len(loaded) + len(zeroed) + 2:
        raise SystemExit("data_layout: libogc_common.ld is not laid out as expected")
    rvl = (args.ldscripts / "rvl.ld").read_text()
    if "INCLUDE libogc_common.ld" not in rvl:
        raise SystemExit("data_layout: rvl.ld does not include libogc_common.ld")
    out = rvl.replace("INCLUDE libogc_common.ld", "\n".join(out_common))
    out = "/* Generated by wii/tools/data_layout.py from devkitPPC's rvl.ld */\n" + out
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(out)
    counts = ", ".join(f"{s} {len(placed.get(s, []))}" for s in LOADED + ZEROED)
    print(f"data_layout: {counts}")


if __name__ == "__main__":
    main()
