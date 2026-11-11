#!/usr/bin/env python3
"""Copies a Dolphin SDK source without its MWCC assembly functions.

The SDK's sources (libs/dolphin/src) mix C with functions written in
CodeWarrior's inline assembly (`asm void f(...) { ... }`, or a C function
around an `asm { ... }` block). GCC reads neither, and the Wii port provides
those functions itself (wii/src/sdk). Every top-level function definition whose
text uses `asm` is left out; the rest of the file is kept line for line, with
#line markers so that diagnostics name the original file.

    strip_asm.py IN OUT [--keep NAME ...] [--drop NAME ...]

--drop also leaves out C functions by name (the port's own replaces them).
"""

import argparse
import re
import sys
from pathlib import Path

ASM = re.compile(r"\basm\b")
NAME = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\s*\(")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def function_name(signature: str) -> str | None:
    names = NAME.findall(signature)
    return names[0] if names else None


def strip(src: str, drop: set[str]) -> str:
    lines = src.split("\n")
    code = strip_comments(src).split("\n")
    out: list[str] = []
    depth = 0
    pending: list[int] = []  # the lines since the last top-level boundary
    block: list[int] = []  # the lines of the current top-level braced block
    in_block = False

    def flush_pending() -> None:
        out.extend(lines[i] for i in pending)
        pending.clear()

    for i, line in enumerate(code):
        stripped = line.strip()
        if not in_block and stripped.startswith("#"):
            # Preprocessor lines at file scope are never part of a signature
            flush_pending()
            out.append(lines[i])
            continue
        opens = line.count("{")
        closes = line.count("}")
        if not in_block:
            pending.append(i)
            if opens > closes or (opens and opens == closes and depth == 0 and "(" in "".join(code[j] for j in pending)):
                if opens == closes:
                    # A one-line block: decide on it now
                    text = "\n".join(code[j] for j in pending)
                    name = function_name(text.split("{")[0])
                    is_func = ")" in text.split("{")[0] and not text.split("{")[0].rstrip().endswith("=")
                    if is_func and (ASM.search(text) or name in drop):
                        out.append(f"#line {i + 2}")
                        pending.clear()
                    else:
                        flush_pending()
                    continue
                in_block = True
                depth = opens - closes
                block = pending[:]
                pending.clear()
                continue
            if stripped.endswith(";") or stripped.endswith("}"):
                flush_pending()
            continue
        block.append(i)
        depth += opens - closes
        if depth <= 0:
            in_block = False
            depth = 0
            text = "\n".join(code[j] for j in block)
            head = text.split("{")[0]
            name = function_name(head)
            is_func = ")" in head and not head.rstrip().endswith("=")
            if is_func and (ASM.search(text) or name in drop):
                out.append(f"#line {i + 2}")
            else:
                out.extend(lines[j] for j in block)
            block = []
    flush_pending()
    out.extend(lines[j] for j in block)
    return "\n".join(out)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--drop", nargs="*", default=[])
    args = parser.parse_args()
    src = args.input.read_text(encoding="utf-8", errors="surrogateescape")
    body = strip(src, set(args.drop))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    header = f'#line 1 "{args.input.resolve().as_posix()}"\n'
    args.output.write_text(header + body, encoding="utf-8", errors="surrogateescape")


if __name__ == "__main__":
    sys.exit(main())
