#!/usr/bin/env python3
"""Is a modded stage file the original stage with another look?

    python wii/tools/stage_check.py ORIGINAL.dat MODDED [MODDED...]   one line each
    python wii/tools/stage_check.py -v ORIGINAL.dat MODDED            and why not

The port may give the game a 20XX stage file in place of its own, online too
(wii/src/mod_stages.c, which makes the same comparison on the Wii; this is
its reference). That is only right when nothing a match depends on differs,
and 20XX's files differ in more than textures: some have other collision
bounds, other item data, other models. So the two files are walked together,
by their structures, and everything must be equal except what is only looks:

- a joint's display objects (meshes, materials, textures) and its
  render-pass flags; material and shape animations; lights, fog, shadows;
  the particles' textures (map_texg); image and palette symbols
- the songs in the stage's rows of grGroundParam (20XX gives its looks their
  own songs, by numbers the original game doesn't have: the port gives the
  game the original's rows, wii/src/mod_stages.c)

Equal, then: the collision (coll_data), the stage's parameters, every
joint's place in its tree and its position, rotation and scale, the joint
animations, the splines, the cameras, the stage's own items (but for their
models' looks), the particles' programs
(map_ptcl: they draw from the game's random numbers), and whatever else the
file has. A part whose structure isn't known here is compared whole.
"""
import argparse
import bisect
import struct
import sys

# A joint's flags that only say how it is drawn (HSD's JOBJ_LIGHTING, TEXGEN,
# SPECULAR, the three render passes and the root's)
JOINT_LOOKS = 0x80 | 0x100 | 0x10000 | 0x40000 | 0x80000 | 0x100000 | 0x70000000
JOINT_PTCL, JOINT_SPLINE = 0x20, 0x4000
ROW = 0x64            # a stage's row in grGroundParam
ROW_SONGS = (4, 0x18)  # its songs


class Differs(Exception):
    pass


class Dat:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        if len(raw) < 0x20:
            raise Differs('not a stage file')
        size, dsize, nrel, nroot, nref = struct.unpack('>5I', raw[:20])
        table = 0x20 + dsize
        names = table + 4 * nrel + 8 * (nroot + nref)
        if size != len(raw) or names > len(raw):
            raise Differs('not a stage file')
        self.d = raw[0x20:table]
        self.size = dsize
        self.rel = set(struct.unpack(f'>{nrel}I', raw[table:table + 4 * nrel]))
        self.roots = {}
        for i in range(nroot):
            off, name = struct.unpack('>II', raw[table + 4 * nrel + 8 * i:][:8])
            end = raw.index(b'\0', names + name)
            self.roots[raw[names + name:end].decode('latin1')] = off
        targets = set(self.roots.values()) | {dsize}
        for r in self.rel:
            targets.add(self.u32(r))
        self.targets = sorted(t for t in targets if t <= dsize)

    def u32(self, off):
        if off + 4 > self.size:
            raise Differs('a structure runs past the file')
        return struct.unpack('>I', self.d[off:off + 4])[0]

    def ptr(self, off):
        """The pointer at `off`: an offset, or None."""
        return self.u32(off) if off in self.rel else None

    def bytes(self, off, n):
        if off + n > self.size:
            raise Differs('a structure runs past the file')
        return self.d[off:off + n]

    def block_end(self, off):
        """Where the data at `off` ends, as far as the file says: the next address something points at."""
        return self.targets[bisect.bisect_right(self.targets, off)]


class Check:
    def __init__(self, a, b):
        self.a, self.b = a, b
        self.seen = set()
        self.songs = []  # (offset in the modded file's data, the original's bytes)

    def pair(self, kind, pa, pb, what):
        """True when this pair of pointers is to be walked (both there, not walked yet)."""
        if (pa is None) != (pb is None):
            raise Differs(f'{what}: there in one file only')
        if pa is None or (kind, pa, pb) in self.seen:
            return False
        self.seen.add((kind, pa, pb))
        return True

    def same(self, oa, ob, n, what):
        if self.a.bytes(oa, n) != self.b.bytes(ob, n):
            raise Differs(what)
        for i in range(0, n, 4):
            if ((oa + i) in self.a.rel) != ((ob + i) in self.b.rel):
                raise Differs(what)

    def whole(self, pa, pb, what):
        """A part of unknown structure: equal block by block, through its pointers."""
        todo = [(pa, pb)]
        while todo:
            pa, pb = todo.pop()
            if not self.pair('whole', pa, pb, what):
                continue
            n = self.a.block_end(pa) - pa
            if n != self.b.block_end(pb) - pb:
                raise Differs(f'{what}: another size')
            for i in range(0, n - n % 4, 4):
                qa, qb = self.a.ptr(pa + i), self.b.ptr(pb + i)
                if (qa is None) != (qb is None):
                    raise Differs(what)
                if qa is not None:
                    todo.append((qa, qb))
                elif self.a.d[pa + i:pa + i + 4] != self.b.d[pb + i:pb + i + 4]:
                    raise Differs(what)
            if self.a.d[pa + n - n % 4:pa + n] != self.b.d[pb + n - n % 4:pb + n]:
                raise Differs(what)

    def joint(self, pa, pb, what):
        todo = [(pa, pb)]
        while todo:
            pa, pb = todo.pop()
            if not self.pair('joint', pa, pb, what):
                continue
            fa, fb = self.a.u32(pa + 4), self.b.u32(pb + 4)
            if (fa | JOINT_LOOKS) != (fb | JOINT_LOOKS):
                raise Differs(f'{what}: a joint\'s flags')
            if self.a.bytes(pa + 0x14, 0x24) != self.b.bytes(pb + 0x14, 0x24):
                raise Differs(f'{what}: a joint\'s position')
            for off in (0, 0x38, 0x3C):  # its class's name, its matrix, its constraints
                qa, qb = self.a.ptr(pa + off), self.b.ptr(pb + off)
                if (qa is None) != (qb is None):
                    raise Differs(f'{what}: a joint')
                if qa is not None:
                    self.whole(qa, qb, f'{what}: a joint\'s data')
            if fa & (JOINT_PTCL | JOINT_SPLINE):
                qa, qb = self.a.ptr(pa + 0x10), self.b.ptr(pb + 0x10)
                if (qa is None) != (qb is None):
                    raise Differs(f'{what}: a joint')
                if qa is not None:
                    self.whole(qa, qb, f'{what}: a joint\'s spline or particles')
            # (else its display objects: looks)
            for off in (8, 0xC):
                qa, qb = self.a.ptr(pa + off), self.b.ptr(pb + off)
                if (qa is None) != (qb is None):
                    raise Differs(f'{what}: the joints\' tree')
                if qa is not None:
                    todo.append((qa, qb))

    def anim_object(self, pa, pb, what):
        """An HSD_AObjDesc: its tracks, and the joint it moves."""
        if not self.pair('aobj', pa, pb, what):
            return
        self.same(pa, pb, 8, f'{what}: an animation')
        fa, fb = self.a.ptr(pa + 8), self.b.ptr(pb + 8)
        while self.pair('fobj', fa, fb, what):
            if self.a.bytes(fa + 4, 12) != self.b.bytes(fb + 4, 12):
                raise Differs(f'{what}: an animation\'s track')
            n = self.a.u32(fa + 4)
            da, db = self.a.ptr(fa + 0x10), self.b.ptr(fb + 0x10)
            if (da is None) != (db is None) or (da is not None and self.a.bytes(da, n) != self.b.bytes(db, n)):
                raise Differs(f'{what}: an animation\'s keys')
            fa, fb = self.a.ptr(fa), self.b.ptr(fb)
        ja, jb = self.a.ptr(pa + 0xC), self.b.ptr(pb + 0xC)
        if (ja is None) != (jb is None):
            raise Differs(f'{what}: an animation\'s joint')
        if ja is not None:
            self.joint(ja, jb, what)

    def anim_joint(self, pa, pb, what):
        todo = [(pa, pb)]
        while todo:
            pa, pb = todo.pop()
            if not self.pair('animjoint', pa, pb, what):
                continue
            if self.a.u32(pa + 0x10) != self.b.u32(pb + 0x10):
                raise Differs(f'{what}: an animation')
            qa, qb = self.a.ptr(pa + 8), self.b.ptr(pb + 8)
            if (qa is None) != (qb is None):
                raise Differs(f'{what}: an animation')
            if qa is not None:
                self.anim_object(qa, qb, what)
            ra, rb = self.a.ptr(pa + 0xC), self.b.ptr(pb + 0xC)
            while self.pair('robjanim', ra, rb, what):
                qa, qb = self.a.ptr(ra + 4), self.b.ptr(rb + 4)
                if (qa is None) != (qb is None):
                    raise Differs(f'{what}: an animation')
                if qa is not None:
                    self.anim_object(qa, qb, what)
                ra, rb = self.a.ptr(ra), self.b.ptr(rb)
            for off in (0, 4):
                qa, qb = self.a.ptr(pa + off), self.b.ptr(pb + off)
                if (qa is None) != (qb is None):
                    raise Differs(f'{what}: the animations\' tree')
                if qa is not None:
                    todo.append((qa, qb))

    def anim_list(self, pa, pb, what):
        """A list of joint animations ended by a null; how many."""
        if (pa is None) != (pb is None):
            raise Differs(f'{what}: its animations')
        count = 0
        while pa is not None:
            qa, qb = self.a.ptr(pa + 4 * count), self.b.ptr(pb + 4 * count)
            if (qa is None) != (qb is None):
                raise Differs(f'{what}: its animations')
            if qa is None:
                if self.a.u32(pa + 4 * count) or self.b.u32(pb + 4 * count):
                    raise Differs(f'{what}: its animations')
                break
            self.anim_joint(qa, qb, what)
            count += 1
        return count

    def array(self, pa, pb, size, what):
        if (pa is None) != (pb is None):
            raise Differs(what)
        if pa is not None and size:
            self.same(pa, pb, size, what)

    def model(self, pa, pb, what):
        """A joint with its animations (DynamicModelDesc); how many animations."""
        ja, jb = self.a.ptr(pa), self.b.ptr(pb)
        if (ja is None) != (jb is None):
            raise Differs(what)
        if ja is not None:
            self.joint(ja, jb, what)
        return self.anim_list(self.a.ptr(pa + 4), self.b.ptr(pb + 4), what)

    def items(self, pa, pb):
        """itemdata: a list of the stage's own items, each a kind and its data (an Article)."""
        a, b = self.a, self.b
        what = 'the stage\'s items'
        n = a.block_end(pa) - pa
        if n != b.block_end(pb) - pb:
            raise Differs(what)
        for i in range(0, n - n % 4, 4):
            ea, eb = a.ptr(pa + i), b.ptr(pb + i)
            if (ea is None) != (eb is None):
                raise Differs(what)
            if ea is None:
                if a.u32(pa + i) != b.u32(pb + i):
                    raise Differs(what)
                continue
            if a.u32(ea) != b.u32(eb):
                raise Differs(what)
            ra, rb = a.ptr(ea + 4), b.ptr(eb + 4)
            if not self.pair('article', ra, rb, what):
                continue
            # its attributes, its own attributes, its hurtboxes, its physics: whole
            for off in (0, 4, 8, 0x14):
                qa, qb = a.ptr(ra + off), b.ptr(rb + off)
                if (qa is None) != (qb is None):
                    raise Differs(what)
                if qa is not None:
                    self.whole(qa, qb, what)
            # its states: a joint animation, a material animation (looks), parameters, a script
            sa, sb = a.ptr(ra + 0xC), b.ptr(rb + 0xC)
            if self.pair('states', sa, sb, what):
                n = a.block_end(sa) - sa
                if n != b.block_end(sb) - sb or n % 0x10:
                    raise Differs(what)
                for s in range(0, n, 0x10):
                    for off in (0, 4, 8, 0xC):
                        qa, qb = a.ptr(sa + s + off), b.ptr(sb + s + off)
                        if (qa is None) != (qb is None):
                            raise Differs(what)
                        if qa is None:
                            if a.u32(sa + s + off) != b.u32(sb + s + off):
                                raise Differs(what)
                        elif off == 0:
                            self.anim_joint(qa, qb, what)
                        elif off != 4:
                            self.whole(qa, qb, what)
            # its model: a joint, and three words
            ma, mb = a.ptr(ra + 0x10), b.ptr(rb + 0x10)
            if self.pair('itemmodel', ma, mb, what):
                self.same(ma + 4, mb + 4, 0xC, what)
                ja, jb = a.ptr(ma), b.ptr(mb)
                if (ja is None) != (jb is None):
                    raise Differs(what)
                if ja is not None:
                    self.joint(ja, jb, what)

    def collision(self, pa, pb):
        what = 'the collision'
        for off in (4, 0xC, 0x28, 0x2C):
            if self.a.u32(pa + off) != self.b.u32(pb + off):
                raise Differs(what)
        self.same(pa + 0x10, pb + 0x10, 0x14, what)
        for off, count, size in ((0, 4, 8), (8, 0xC, 0x10), (0x24, 0x28, 0x28)):
            self.array(self.a.ptr(pa + off), self.b.ptr(pb + off), self.a.u32(pa + count) * size, what)

    def parameters(self, pa, pb):
        what = 'the stage\'s parameters'
        self.same(pa, pb, 0xB0, what)
        self.same(pa + 0xB4, pb + 0xB4, 0x28, what)
        ra, rb = self.a.ptr(pa + 0xB0), self.b.ptr(pb + 0xB0)
        if (ra is None) != (rb is None):
            raise Differs(what)
        for i in range(self.a.u32(pa + 0xB4) if ra is not None else 0):
            oa, ob = ra + i * ROW, rb + i * ROW
            self.same(oa, ob, ROW_SONGS[0], what)
            self.same(oa + ROW_SONGS[1], ob + ROW_SONGS[1], ROW - ROW_SONGS[1], what)
            songs = self.a.bytes(oa + ROW_SONGS[0], ROW_SONGS[1] - ROW_SONGS[0])
            if songs != self.b.bytes(ob + ROW_SONGS[0], ROW_SONGS[1] - ROW_SONGS[0]):
                self.songs.append((ob + ROW_SONGS[0], songs))

    def map_head(self, pa, pb):
        a, b = self.a, self.b
        for off in (4, 0xC, 0x14):
            if a.u32(pa + off) != b.u32(pb + off):
                raise Differs('the map: its counts')
        # the points: a joint each, and pairs of joint numbers
        ea, eb = a.ptr(pa), b.ptr(pb)
        if (ea is None) != (eb is None):
            raise Differs('the map\'s points')
        for i in range(a.u32(pa + 4) if ea is not None else 0):
            oa, ob = ea + i * 0xC, eb + i * 0xC
            ja, jb = a.ptr(oa), b.ptr(ob)
            if (ja is None) != (jb is None) or a.u32(oa + 8) != b.u32(ob + 8):
                raise Differs('the map\'s points')
            if ja is not None:
                self.joint(ja, jb, 'the map\'s points')
            self.array(a.ptr(oa + 4), b.ptr(ob + 4), a.u32(oa + 8) * 4, 'the map\'s points')
        # the models
        ea, eb = a.ptr(pa + 8), b.ptr(pb + 8)
        if (ea is None) != (eb is None):
            raise Differs('the map\'s models')
        for i in range(a.u32(pa + 0xC) if ea is not None else 0):
            oa, ob = ea + i * 0x34, eb + i * 0x34
            what = f'the map\'s model {i}'
            anims = self.model(oa, ob, what)
            for off in (0x10, 0x14):  # its camera and the camera's animation
                qa, qb = a.ptr(oa + off), b.ptr(ob + off)
                if (qa is None) != (qb is None):
                    raise Differs(f'{what}: its camera')
                if qa is not None:
                    self.whole(qa, qb, f'{what}: its camera')
            if a.u32(oa + 0x24) != b.u32(ob + 0x24) or a.u32(oa + 0x30) != b.u32(ob + 0x30):
                raise Differs(what)
            self.array(a.ptr(oa + 0x20), b.ptr(ob + 0x20), a.u32(oa + 0x24) * 6, what)
            self.array(a.ptr(oa + 0x28), b.ptr(ob + 0x28), anims, what)
            self.array(a.ptr(oa + 0x2C), b.ptr(ob + 0x2C), a.u32(oa + 0x30) * 2, what)
            # (+8, +C: material and shape animations; +18, +1C: lights, fog)
        # the splines
        ea, eb = a.ptr(pa + 0x10), b.ptr(pb + 0x10)
        if (ea is None) != (eb is None):
            raise Differs('the map\'s splines')
        for i in range(a.u32(pa + 0x14) if ea is not None else 0):
            qa, qb = a.ptr(ea + 4 * i), b.ptr(eb + 4 * i)
            if (qa is None) != (qb is None):
                raise Differs('the map\'s splines')
            if qa is not None:
                self.whole(qa, qb, 'the map\'s splines')
        # (+18: lights; +20: shadows; +28: the materials shadows fall on)

    def run(self):
        a, b = self.a, self.b
        for name, pa in a.roots.items():
            if name not in b.roots:
                raise Differs(f'no {name}')  # (the game finds its files' parts by these names)
            pb = b.roots[name]
            if name == 'coll_data':
                self.collision(pa, pb)
            elif name == 'grGroundParam':
                self.parameters(pa, pb)
            elif name == 'map_head':
                self.map_head(pa, pb)
            elif name == 'itemdata':
                self.items(pa, pb)
            elif name == 'quake_model_set':
                self.model(pa, pb, 'the quake\'s model')
            elif name.endswith('_matanim_joint') or name.endswith('_shapeanim_joint'):
                pass
            elif name.endswith('_animjoint'):
                self.anim_joint(pa, pb, name)
            elif name.endswith('_joint'):
                self.joint(pa, pb, name)
            elif name in ('map_texg', 'map_plit') or name.endswith(('_image', '_tlut', '_tlut_desc', '_image_desc')):
                pass
            else:
                self.whole(pa, pb, name)
        return self.songs


def check(original, modded):
    """None when `modded` is `original` with another look, else what differs."""
    try:
        Check(Dat(original), Dat(modded)).run()
    except Differs as e:
        return str(e)
    except (struct.error, IndexError, ValueError):
        return 'not a stage file'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', action='store_true', help='say what differs')
    ap.add_argument('original')
    ap.add_argument('modded', nargs='+')
    args = ap.parse_args()
    bad = 0
    for path in args.modded:
        why = check(args.original, path)
        bad += why is not None
        print(f'{"differs" if why else "looks  "}  {path}' + (f'  ({why})' if why and args.v else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
