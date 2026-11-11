#!/usr/bin/env python3
"""Writes the byte tables configure.py's dtk extracts from main.dol (config.yml's
`extract:`) as the raw .inc headers the game sources #include."""
import struct
import sys
from pathlib import Path

# symbol, header, address, size (config/GALE01/symbols.txt)
TABLES = [
    ("sysdolphin/baselib/sislib_font.inc", 0x8040CD40, 0x23E00),
    ("sysdolphin/baselib/debug_font.inc", 0x804088B8, 0x1C00),
]


def read(dol: bytes, addr: int, size: int) -> bytes:
    offs = struct.unpack(">18I", dol[0:72])
    addrs = struct.unpack(">18I", dol[72:144])
    sizes = struct.unpack(">18I", dol[144:216])
    for off, base, n in zip(offs, addrs, sizes):
        if n and base <= addr and addr + size <= base + n:
            return dol[off + addr - base : off + addr - base + size]
    sys.exit(f"extract_incs: 0x{addr:08X}+0x{size:X} is in no section of the DOL")


def main() -> None:
    dol = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2])
    for header, addr, size in TABLES:
        data = read(dol, addr, size)
        lines = [
            "".join(f"0x{b:02X}, " for b in data[i : i + 16]).rstrip() + "\n"
            for i in range(0, len(data), 16)
        ]
        path = out / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("".join(lines))


if __name__ == "__main__":
    main()
