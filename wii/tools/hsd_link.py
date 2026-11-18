"""Slippi 3.7.0's linked game files, made as VCDIFF patches against the disc.

Since 3.7.0, two of Slippi's menu files (GameSetup_gui.dat, SlippiCSS_gui.dat)
hold none of Nintendo's art: each piece of it is a reference by name,
"slp:<disc file>:<root>/<hops>:at=..:sha=..[:cut=..]", which Slippi Dolphin
fills in from the player's own disc as it serves the file (the Rust crate
hsd's resolve, slippi-rust-extensions 934f373: follow the path in the disc
file, copy that structure and everything it points to into the archive,
point the reference at it).

The Wii does the same from the release, without any of Nintendo's bytes in
it: this resolves the file on a PC from a disc, then writes the result as a
VCDIFF patch whose source is the disc files it used, one after the other.
Every byte that came from the disc is a COPY from there; the rest (Slippi's
own data, the new offsets, the archive's tables) is in the patch. The
launcher applies it as it does Slippi's own patches (wii/src/launcher/files.c,
"link" lines of the manifest).

    python wii/tools/hsd_link.py Melee.iso SlippiCSS_gui.dat out/
"""
import bisect
import hashlib
import struct
import sys
from collections import deque
from pathlib import Path

HEADER, ALIGN, END = 0x20, 32, 0xFFFFFFFF


def _be(b, o):
    return struct.unpack_from('>I', b, o)[0]


class Ext:
    """One structure of an archive's data: its bytes (pointer fields
    zeroed), where it starts modulo 32, its pointers (field offset, target
    extent), and where it came from (file or None, offset in its data)."""
    __slots__ = ('data', 'phase', 'ptrs', 'origin', 'file')

    def __init__(self, data, phase, ptrs, origin, file=None):
        self.data, self.phase, self.ptrs, self.origin, self.file = data, phase, ptrs, origin, file


class Archive:
    pass


def _cstr(t, o):
    return t[o:t.index(b'\0', o)].decode('latin1')


def parse(b, file=None):
    """An HSD archive, split at every pointer target into extents."""
    fs, ds, rc, roc, refc = struct.unpack_from('>5I', b, 0)
    if fs != len(b):
        raise ValueError(f'archive size {fs} != {len(b)}')
    data = b[HEADER:HEADER + ds]
    ro = HEADER + ds
    rto = ro + rc * 4
    rfo = rto + roc * 8
    strs = b[rfo + refc * 8:]
    fields, bounds = [], {0, ds}
    for i in range(rc):
        f = _be(b, ro + i * 4)
        t = _be(data, f)
        bounds.add(t)
        fields.append((f, t))
    roots = []
    for i in range(roc):
        t = _be(b, rto + i * 8)
        roots.append((_cstr(strs, _be(b, rto + i * 8 + 4)), t))
        bounds.add(t)
    refs = []
    for i in range(refc):
        at = _be(b, rfo + i * 8)
        name = _cstr(strs, _be(b, rfo + i * 8 + 4))
        chain = []
        while True:
            chain.append(at)
            nxt = _be(data, at)
            if nxt == END:
                break
            at = nxt
        refs.append((name, chain))
    end_target = any(t == ds for _, t in fields) or any(t == ds for _, t in roots)
    starts = sorted(x for x in bounds if x < ds or (end_target and x == ds))
    index = {s: i for i, s in enumerate(starts)}
    exts = []
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else ds
        exts.append(Ext(bytearray(data[s:e]), s % ALIGN, [], s, file))

    def locate(f):
        i = bisect.bisect_right(starts, f) - 1
        return i, f - starts[i]

    for f, t in fields:
        i, w = locate(f)
        exts[i].data[w:w + 4] = bytes(4)
        exts[i].ptrs.append((w, index[t]))
    a = Archive()
    a.version, a.tail, a.exts = b[0x14:0x18], b[0x18:0x20], exts
    a.roots = [(n, index[t]) for n, t in roots]
    a.refs = []
    for name, chain in refs:
        locs = []
        for at in chain:
            i, w = locate(at)
            exts[i].data[w:w + 4] = bytes(4)
            locs.append((i, w))
        a.refs.append((name, locs))
    return a


def layout(a):
    offs, c = [], 0
    for e in a.exts:
        c += (e.phase + ALIGN - c % ALIGN) % ALIGN
        offs.append(c)
        c += len(e.data)
    return offs, c


def serialize(a):
    """The archive's bytes, and where each extent went (offset in the file)."""
    offs, ds = layout(a)
    data = bytearray(ds)
    relocs = []
    for i, e in enumerate(a.exts):
        base = offs[i]
        data[base:base + len(e.data)] = e.data
        for f, t in e.ptrs:
            struct.pack_into('>I', data, base + f, offs[t])
            relocs.append(base + f)
    relocs.sort()
    heads = []
    for name, locs in a.refs:
        for n, (ei, f) in enumerate(locs):
            v = offs[locs[n + 1][0]] + locs[n + 1][1] if n + 1 < len(locs) else END
            struct.pack_into('>I', data, offs[ei] + f, v)
        heads.append(offs[locs[0][0]] + locs[0][1])
    strs, root_names, ref_names = bytearray(), [], []
    for n, _ in a.roots:
        root_names.append(len(strs))
        strs += n.encode('latin1') + b'\0'
    for n, _ in a.refs:
        ref_names.append(len(strs))
        strs += n.encode('latin1') + b'\0'
    size = HEADER + ds + len(relocs) * 4 + (len(a.roots) + len(a.refs)) * 8 + len(strs)
    out = bytearray(struct.pack('>5I', size, ds, len(relocs), len(a.roots), len(a.refs)) + a.version + a.tail)
    out += data
    for r in relocs:
        out += struct.pack('>I', r)
    for (n, e), s in zip(a.roots, root_names):
        out += struct.pack('>II', offs[e], s)
    for h, s in zip(heads, ref_names):
        out += struct.pack('>II', h, s)
    return bytes(out + strs), [HEADER + o for o in offs]


def _ext_hash(a, i, memo, busy, skip=None):
    if skip is None and i in memo:
        return memo[i]
    if i in busy:
        return hashlib.sha1(b'cycle').digest()
    busy.add(i)
    e = a.exts[i]
    children = [(f, _ext_hash(a, t, memo, busy)) for f, t in sorted(e.ptrs) if not (skip and f in skip)]
    h = hashlib.sha1()
    h.update(bytes(e.data).rstrip(b'\0'))
    for f, c in children:
        h.update(struct.pack('>I', f))
        h.update(c)
    busy.discard(i)
    d = h.digest()
    if skip is None:
        memo[i] = d
    return d


def _parse_link(sym):
    parts = sym[4:].split(':')
    file, anchor = parts[0], parts[1]
    opts = dict(p.split('=', 1) for p in parts[2:])
    root, hops = anchor.split('/', 1)
    hop_list = []
    for h in hops.split('/'):
        if not h:
            continue
        f, _, n = h.partition('*')
        hop_list += [int(f, 16)] * (int(n) if n else 1)
    cut = [int(x, 16) for x in opts['cut'].split(',')] if 'cut' in opts else []
    return file, root, hop_list, cut, opts.get('sha')


def resolve(a, load):
    """Fills in every "slp:" reference from load(disc file name). Returns the
    new archive and the disc files it used (sorted). Raises ValueError when
    a reference's structure is not the one Slippi expects (sha)."""
    out = Archive()
    out.version, out.tail = a.version, a.tail
    out.exts = [Ext(bytearray(e.data), e.phase, list(e.ptrs), e.origin, e.file) for e in a.exts]
    out.roots, out.refs = list(a.roots), []
    cache = {}
    for name, locs in a.refs:
        if not name.startswith('slp:'):
            out.refs.append((name, locs))
            continue
        file, root, hops, cut, sha = _parse_link(name)
        if file not in cache:
            data = load(file)
            if data is None:
                raise ValueError(f'{file} is not on the disc')
            cache[file] = parse(data, file)
        src = cache[file]
        cur = dict(src.roots)[root]
        for h in hops:
            cur = next(t for f, t in src.exts[cur].ptrs if f == h)
        if sha and _ext_hash(src, cur, {}, set(), set(cut) if cut else None).hex()[:8] != sha:
            raise ValueError(f'{name}: the disc\'s structure is not the expected one')
        top, seen, order, q = cur, {cur}, [], deque([cur])
        while q:
            i = q.popleft()
            order.append(i)
            for f, t in src.exts[i].ptrs:
                if (i == top and f in cut) or t in seen:
                    continue
                seen.add(t)
                q.append(t)
        ordered = sorted(order, key=lambda i: src.exts[i].origin)
        new = {}
        for i in ordered:
            new[i] = len(out.exts)
            s = src.exts[i]
            out.exts.append(Ext(bytearray(s.data), s.phase, [], s.origin, file))
        for i in ordered:
            out.exts[new[i]].ptrs = [(f, new[t]) for f, t in src.exts[i].ptrs
                                     if not (i == top and f in cut) and t in new]
        for e, f in locs:
            out.exts[e].ptrs.append((f, new[top]))
    return out, sorted(cache)


# ---- VCDIFF (RFC 3284) encoding --------------------------------------------

def _varint(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


# Any byte that came from the disc is copied from it, however short the run:
# the patch holds none of the disc's bytes.
MIN_COPY = 1


def encode(target, source, hints):
    """A VCDIFF delta making `target` from `source`, default code table, one
    window. `hints`: (start, end, source offset) for target ranges that came
    from the source; there each run of equal bytes is a COPY and only the
    bytes that differ (new pointer values) an ADD; outside them everything
    is an ADD (or a RUN of one byte)."""
    ops = []  # ('lit', bytes) or ('copy', src, n)

    def literal(b):
        if ops and ops[-1][0] == 'lit':
            ops[-1] = ('lit', ops[-1][1] + b)
        else:
            ops.append(('lit', bytes(b)))

    pos = 0
    for start, end, src in sorted(hints):
        if start > pos:
            literal(target[pos:start])
        i = start
        while i < end:
            j = i
            while j < end and target[j] == source[src + j - start]:
                j += 1
            if j - i >= MIN_COPY:
                ops.append(('copy', src + i - start, j - i))
                i = j
            else:
                k = max(j, i + 1)
                literal(target[i:k])
                i = k
        pos = end
    if pos < len(target):
        literal(target[pos:])

    data, inst, addr = bytearray(), bytearray(), bytearray()
    for op in ops:
        if op[0] == 'copy':
            inst.append(19)  # COPY, size follows, mode 0 (absolute address)
            inst += _varint(op[2])
            addr += _varint(op[1])
            continue
        b = op[1]
        i = 0
        while i < len(b):
            j = i
            while j < len(b) and b[j] == b[i]:
                j += 1
            if j - i >= 8:
                inst.append(0)  # RUN, size follows
                inst += _varint(j - i)
                data.append(b[i])
                i = j
                continue
            # an ADD up to the next long run
            k = i
            while k < len(b):
                m = k
                while m < len(b) and b[m] == b[k]:
                    m += 1
                if m - k >= 8:
                    break
                k = m
            inst.append(1)  # ADD, size follows
            inst += _varint(k - i)
            data += b[i:k]
            i = k
    body = (_varint(len(target)) + b'\0' + _varint(len(data)) + _varint(len(inst)) + _varint(len(addr))
            + bytes(data) + bytes(inst) + bytes(addr))
    window = b'\x01' + _varint(len(source)) + _varint(0) + _varint(len(body)) + body
    return b'\xd6\xc3\xc4\x00\x00' + window


def make_patch(linked, disc_file):
    """Resolves `linked` against the disc (disc_file(name) -> bytes) and
    returns (the file, the disc files it needs in order, the patch)."""
    resolved, files = resolve(parse(linked), disc_file)
    target, offsets = serialize(resolved)
    starts, source = {}, bytearray()
    for name in files:
        starts[name] = len(source)
        source += disc_file(name)
    hints = []
    for e, off in zip(resolved.exts, offsets):
        if e.file is not None and len(e.data):
            hints.append((off, off + len(e.data), starts[e.file] + HEADER + e.origin))
    delta = encode(target, bytes(source), hints)
    return target, files, delta


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import slippi_files
    from vcdiff import decode
    disc = slippi_files.open_disc(sys.argv[1])
    linked = Path(sys.argv[2])
    out = Path(sys.argv[3])
    target, files, delta = make_patch(linked.read_bytes(), disc.file)
    source = b''.join(disc.file(n) for n in files)
    assert decode(source, delta) == target
    out.mkdir(parents=True, exist_ok=True)
    (out / linked.name).write_bytes(target)
    (out / (linked.name + '.diff')).write_bytes(delta)
    print(f'{linked.name}: {len(target)} bytes from {"+".join(files)}; patch {len(delta)} bytes')


if __name__ == '__main__':
    main()
