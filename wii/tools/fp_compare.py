#!/usr/bin/env python3
"""Compare the floating-point arithmetic of the game's functions as MWCC built
them (the GameCube build, build/GALE01/src) and as the Wii build has them.

    fp_compare.py [--objdump OBJDUMP] [--all] [FILTER...]

Online play runs the Wii build in lockstep with the GameCube's game (on Slippi
Dolphin), so the two have to compute the same results bit for bit: each fused
multiply-add, each rounding to single precision, each estimate. For each
function this counts the instructions that round (fused ops, multiplies, adds,
divides, frsp, conversions, estimates, paired singles) in both builds and
lists the functions whose counts differ. Inlining differs between the
compilers, so a difference is a lead, not a verdict.
"""

import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MWCC = os.path.join(ROOT, "build", "GALE01", "src")
GCC = os.path.join(ROOT, "build", "wii", "CMakeFiles", "melee_game.dir", "src")

FUSED = {"fmadd", "fmadds", "fmsub", "fmsubs", "fnmadd", "fnmadds", "fnmsub", "fnmsubs"}
COUNTED = FUSED | {
    "fmul", "fmuls", "fadd", "fadds", "fsub", "fsubs", "fdiv", "fdivs", "frsp",
    "fctiwz", "fctiw", "fres", "frsqrte", "fsel",
}

LINE = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*(\S+)")
FUNC = re.compile(r"^[0-9a-f]+ <([^>]+)>:")


def ops(objdump, path):
    """{function: Counter(mnemonic)}"""
    out = subprocess.run([objdump, "-d", "-M", "750cl", path], capture_output=True,
                         text=True).stdout
    funcs = {}
    cur = None
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            cur = funcs.setdefault(m.group(1), collections.Counter())
            continue
        m = LINE.match(line)
        if m and cur is not None:
            op = m.group(1).rstrip(".")
            if op in COUNTED or op.startswith("ps_") or op.startswith("psq_"):
                cur[op] += 1
    return funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", default="/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump")
    ap.add_argument("--all", action="store_true", help="every difference, not only fused ops")
    ap.add_argument("filter", nargs="*")
    args = ap.parse_args()

    total = 0
    differ = collections.Counter()
    for dirpath, _, files in os.walk(MWCC):
        for name in sorted(files):
            if not name.endswith(".o"):
                continue
            mw = os.path.join(dirpath, name)
            rel = os.path.relpath(mw, MWCC)
            if args.filter and not any(f in rel for f in args.filter):
                continue
            gc = os.path.join(GCC, rel)
            if not os.path.exists(gc):
                continue
            a = ops(args.objdump, mw)
            b = ops(args.objdump, gc)
            for fn, ca in sorted(a.items()):
                if not ca:
                    continue
                total += 1
                cb = b.get(fn)
                if cb is None:
                    continue  # inlined or renamed in the Wii build
                fa = sum(v for k, v in ca.items() if k in FUSED)
                fb = sum(v for k, v in cb.items() if k in FUSED)
                if ca == cb or (not args.all and fa == fb):
                    continue
                diff = {k: (ca[k], cb[k]) for k in set(ca) | set(cb) if ca[k] != cb[k]}
                kind = "fused" if fa != fb else "other"
                differ[kind] += 1
                print(f"{rel}: {fn}: " + ", ".join(
                    f"{k} {x}->{y}" for k, (x, y) in sorted(diff.items())))
    print(f"{total} functions with floating-point arithmetic; differing: "
          f"{dict(differ)}", file=sys.stderr)


if __name__ == "__main__":
    main()
