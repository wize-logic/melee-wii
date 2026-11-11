#!/usr/bin/env python3
"""Lists 20XX's debug menu, read from a 20XX disc image.

    python wii/tools/mod_menu.py 20XX.iso            every list, item by item
    python wii/tools/mod_menu.py 20XX.iso --outline  each distinct list once, labels and choices

The menu is not in 20XX's executable: its lists are data in MnSlChr.0sd (the
character select's file, which 20XX loads at a fixed address, 0x80BEC720, and
branches into). A list is items of 32 bytes, ended by one of kind 9:

    +0x00 kind      0 text, 1 a submenu or an action, 2 a choice of names,
                    3 an integer, 4 a signed one, 8 a float
    +0x04 callback  code run when the item is used (0: none)
    +0x08 label     text
    +0x0C names     for a choice: a table of text pointers
    +0x10 variable  where the value is (most in the executable's data, where
                    the original game's own debug menu was: 0x803FA124 on)
    +0x14 min, +0x18 max, +0x1C step (floats)

The values are all there is to an option here; the code that acts on them is
in 20XX's patches to the game (wii/docs/20xx.md).
"""
import argparse
import collections
import struct
import sys

MNSLCHR_BASE = 0x80BEC720


def disc_files(iso):
    """{name: (offset, size)} of a GameCube image's files, and its executable's offset."""
    iso.seek(0x420)
    dol, fst_off, fst_size = struct.unpack('>III', iso.read(12))
    iso.seek(fst_off)
    fst = iso.read(fst_size)
    count = struct.unpack('>I', fst[8:12])[0]
    names = fst[count * 12:]
    out = {}
    for i in range(1, count):
        entry = fst[i * 12:i * 12 + 12]
        if entry[0] == 0:
            at = struct.unpack('>I', entry[0:4])[0] & 0xFFFFFF
            out[names[at:names.index(b'\0', at)].decode('latin1')] = struct.unpack('>II', entry[4:12])
    return out, dol


class Memory:
    """The executable's sections and the menu's file, by address."""

    def __init__(self, path):
        with open(path, 'rb') as iso:
            files, dol = disc_files(iso)
            if 'MnSlChr.0sd' not in files:
                sys.exit(f'{path}: no MnSlChr.0sd (not a 20XX image?)')
            iso.seek(dol)
            head = iso.read(0x100)
            offsets = struct.unpack('>18I', head[0:72])
            addresses = struct.unpack('>18I', head[0x48:0x48 + 72])
            sizes = struct.unpack('>18I', head[0x90:0x90 + 72])
            self.parts = []
            for offset, address, size in zip(offsets, addresses, sizes):
                if size:
                    iso.seek(dol + offset)
                    self.parts.append((address, iso.read(size)))
            offset, size = files['MnSlChr.0sd']
            iso.seek(offset)
            self.file = (MNSLCHR_BASE, iso.read(size))
            self.parts.append(self.file)

    def read(self, address, size):
        for base, data in self.parts:
            if base <= address and address + size <= base + len(data):
                return data[address - base:address - base + size]
        return None

    def text(self, address):
        if address < 0x80003100:
            return None
        raw = self.read(address, 1)
        out = b''
        while raw and raw != b'\0' and len(out) < 80:
            out += raw
            raw = self.read(address + len(out), 1)
        try:
            s = out.decode('ascii')
        except UnicodeDecodeError:
            return None
        return s if s and all(32 <= ord(c) < 127 for c in s) else None


Item = collections.namedtuple('Item', 'address kind callback label names variable value low high step choices')


def read_list(mem, address):
    """The items of a list at `address`, or None if there is none."""
    items = []
    for k in range(64):
        raw = mem.read(address + k * 0x20, 0x20)
        if raw is None:
            return None
        kind, callback, label, names, variable = struct.unpack('>IIIII', raw[:20])
        low, high, step = struct.unpack('>fff', raw[20:32])
        if kind == 9:
            return items or None
        if kind > 9 or (label and mem.text(label) is None) or (callback and not 0x80003100 <= callback < 0x81800000):
            return None
        choices = []
        if names and kind in (2, 3):
            for c in range(int(high) if 0 < high < 64 else 0):
                p = mem.read(names + 4 * c, 4)
                t = mem.text(struct.unpack('>I', p)[0]) if p else None
                if t is None:
                    break
                choices.append(t.strip())
        v = mem.read(variable, 4) if variable else None
        items.append(Item(address + k * 0x20, kind, callback, (mem.text(label) or '').strip(), names, variable,
                          struct.unpack('>I', v)[0] if v else None, low, high, step, choices))
    return None


def all_lists(mem):
    """{address: items} of every list in the menu's file."""
    base, data = mem.file
    out = {}
    address = base
    while address < base + len(data):
        items = read_list(mem, address)
        if items and len(items) >= 2 and any(i.label for i in items):
            out[address] = items
            address += len(items) * 0x20
        else:
            address += 4
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image')
    ap.add_argument('--outline', action='store_true', help='each distinct list once, labels and choices only')
    args = ap.parse_args()
    lists = all_lists(Memory(args.image))
    print(f'{len(lists)} lists, {sum(len(v) for v in lists.values())} items')
    seen = {}
    for address, items in lists.items():
        if args.outline:
            key = tuple(i.label for i in items)
            if key in seen:
                seen[key] += 1
                continue
            seen[key] = 1
            print(f'\n== {address:08x} ({len(items)} items)')
            for i in items:
                if i.label:
                    tail = '[' + ' | '.join(i.choices) + ']' if i.choices else (f'up to {i.high:g}' if i.kind in (3, 4, 8) else '')
                    print(f'   {"-> " if i.kind == 1 else "   "}{i.label:36} {tail}')
        else:
            print(f'\nLIST {address:08x}')
            for i in items:
                value = f' ={i.value:08x}' if i.value is not None else ''
                print(f'  {i.address:08x} kind {i.kind} {i.label[:34]:34} callback {i.callback:08x} '
                      f'variable {i.variable:08x}{value} max {i.high:g} {" | ".join(i.choices)[:100]}')


if __name__ == '__main__':
    main()
