# How the port works

The game is not recompiled. The decompilation's matching build (`build/GALE01/main.dol`) is put in
the Wii's memory where a GameCube has it, the SDK functions a Wii can't run branch to the port's
own, and the port runs beside it over libogc. For building: [the README](../README.md#building).

## The parts

| | |
|---|---|
| `cmake/Wii.cmake` | The build: the SDK's own sources where they run unchanged (`melee_sdk`), the port (`melee_port`, `melee_port_ogc`), Slippi (`melee_slippi`), wiilib, the DOL |
| `wii/tools/orig_image.py` | The game's image: the matching DOL's sections at their addresses, the branches from the SDK functions the port replaces (`tramp.S`), and the linker script |
| `wii/src/boot_mem2.S`, `main.c` | The start: the port's copy to MEM2, the game's image, the launcher, then the game's `main` |
| `wii/src/os*.c`, `cache.c`, `libc_compat.c`, `stubs.c` | The SDK's OS on libogc: threads, alarms, interrupts, the arena |
| `wii/src/vi.c`, `gx_wii.c`, `thp.c` | Video: VI on libogc's, what the Wii's GPU reads that a GameCube's ignores, the movie decoder's assembly parts |
| `wii/src/ar.c`, `ax_wii.c` | Sound: ARAM as 16 MiB of MEM2, AX's sample addresses for it |
| `wii/src/pad.c`, `classic.c` | Controllers |
| `wii/src/dvd.c`, `card.c`, `exi.c` | The disc image, the memory card as `.gci` files, EXI for Slippi's device in slot B |
| `wii/src/gecko.c` | Gecko codes, applied before the game starts |
| `wii/src/stall.c` | Crash and stall reports; the floating-point accesses a 750 doesn't do |
| `wii/src/launcher`, `launcher*.c` | The launcher: a software-drawn screen on the XFB (DejaVu Sans, `wii/tools/make_font.py`), settings, Slippi's files (VCDIFF patches) |
| `wii/src/slippi`, `mmu_track.c`, `copy.c`, `fused.c`, `wifi.c`, `overlay.c` | Slippi Online: [netplay.md](netplay.md) |
| `wii/src/mod_*.c`, `stage_check.c` | 20XX: [20xx.md](20xx.md) |

In the shared sources (`src/`, `libs/dolphin`) the port's changes are behind `TARGET_WII`, or
`#ifndef __MWERKS__` where any other compiler needs them (C for MWCC's assembly, `volatile` reads
of what interrupts set). The matching build is unchanged.

## Memory

| Where | What |
|---|---|
| 0x80003100–0x804DEC00 | The game's DOL, at the GameCube's addresses |
| 0x804E2800–0x804EEC00 | The port's small data and boot stub, above what Slippi's codes use, in the GameCube's main stack (the game's main thread runs on libogc's) |
| 0x804EEC00– | The game's arena, starting where a GameCube's does |
| top of MEM1 | The SDK's data the DSP reads, and libogc's own MEM1 |
| MEM2, from 0x90002000 | The port's code and data, its bss, newlib's heap, ARAM, the savestates |

- **The port is loaded in MEM1 and copied up.** A loader then needs nothing but MEM1;
  `melee_boot` copies the port to MEM2 and starts libogc. The linker adds long branches where the
  port's code calls the game's (`--relax`).
- **The Wii's globals.** The Wii keeps globals from 0x80003100 on, where the game's `memset`,
  `memcpy` and `__fill_mem` are, and IOS writes some of them while a program runs. The port leaves
  that memory to the Wii and runs the three from a copy in the "island" (MetroTRK's unused code,
  which also holds the veneers to MEM2 and Gecko codes' inserted code).
- **Replaced functions.** `orig_image.py` puts a branch to the port's function at the start of
  each SDK function it replaces. `--keep` leaves a unit as MWCC built it (GX, MTX, OSAlloc,
  PadClamp, AXFX); `--override` and `--redirect` in `cmake/Wii.cmake` take over single functions
  and single calls.

## What a Wii does that Dolphin doesn't show

- In Wii mode the disc, serial, EXI and audio interfaces answer only at `0xCD006000`. Dolphin also
  answers at their GameCube addresses (`0xCC006000`), so a loop waiting on one of those registers
  never ends on a Wii. The SDK sources built for the port use the Wii's
  (`libs/dolphin/include/dolphin/hw_regs.h`).
- EXI DMA reaches MEM1 only: a buffer in MEM2 is written at its MEM1 alias.
- The 750 doesn't do a floating-point load or store at an address that isn't a multiple of 4: it
  takes an alignment exception. Dolphin does them, and Slippi's codes have some. The port's
  exception handler does them byte by byte (`stall.c`).
- The audio interface's DMA address takes 13 bits on a Wii (MEM2), 10 on a GameCube; the video
  interface spreads a 16:9 picture over 704 pixels of the line.
- The DSP and the CPU's data cache are closer to a Wii's with `Dolphin.Core.DSPHLE=False` and
  `Dolphin.Core.AccurateCPUCache=True`.

Dolphin does show these two, but they have to be measured, not heard: compare its audio dump
(`[DSP] DumpAudio`) with the GameCube game's, sample by sample (both run the same DSP code, so the
music matches exactly).

- AX's output buffers go on 32 bytes: the audio interface's DMA drops an address's low five bits,
  and a buffer 16 bytes off plays 4 samples from before it every 10 ms (a buzz under everything).
  MWCC's link put them there; GCC has to be told (`AX_DMA_ALIGN`).
- The SD card is faster than the disc: a music stream's header and its first block can arrive in
  the same 5 ms audio frame, which the SDK's AX syncs so that the voice's loop flag is lost, and
  the music stops after its first block. `ax_wii.c` keeps the whole address block.

## Crashes and stalls

- **A crash** (`stall.c`) is written to `sd:/apps/melee/logs/log.txt` and shown on the screen: the
  exception, where, the registers, the code around it, the calls, the hardware's state.
- **A stall** (no new frame for 5 s) is logged once: what the game's main thread waits in, its
  calls, and the state of the GPU, VI, DSP, audio interface and disc.
- The log also goes to Dolphin's OSREPORT log.

## Testing

Dolphin runs the DOL with an SD card image or folder. Its Wii sockets are the PC's: Slippi
Launcher on the same PC can mirror `127.0.0.1`.

Files in `sd:/apps/melee/` that tests look for:

| File | |
|---|---|
| `input.txt` | Controller 1 played back from a script (`pad.c` describes its lines) |
| `dvdlog` | Each disc read and lookup, logged |
| `crashtest`, `stalltest` | A crash, or a stop, at the start: for `stall.c`'s reports |
| `exittest` | Leaves as the reset button does, 20 s in |
| `mmutest`, `lowmemtest` | The page tracking's self-test; the Wii's globals written over |
| `nofused`, `fusedcount` | [netplay.md](netplay.md#slippi-dolphins-rounding) |

Tools on the PC (`wii/tools`):

| | |
|---|---|
| `gamelog.py` | Reads the Diagnostics records |
| `fp_compare.py` | Compares the floating-point code of the game's functions as MWCC built them and as GCC does |
| `stage_check.py`, `mod_menu.py` | 20XX: the file check's reference, and a lister of its menu |
| `slippi_files.py` | Makes Slippi's SD card files on a PC, from your disc |
| `make_font.py`, `make_icon.py` | The launcher's font and the Homebrew Channel icon (run by hand; their output is checked in) |

**`MELEE_SYNCTEST`** (`src/melee/gm/synctest.inc`) is a determinism test: defined in the matching
build, every frame of a match logs a hash of the fighters' state, and the random seed is set as the
title screen and a match begin, so the title's demo matches play the same everywhere. A Wii build
made from that DOL and the GameCube game in Dolphin must log the same lines.

Two more defines, set by hand, are for rollback's tests: `MELEE_DIRTY_DIAG` (how much of the
savestates' memory changes each frame) and `MELEE_WATCH_DIAG` (a thread that watches the game's
code for changes).

## The GCC build

`-DMELEE_ORIG_CODE=OFF` compiles the game's C sources with GCC instead of placing the matching
build. It has no launcher, Slippi Online or Gecko codes, and its floating point is not the
GameCube's, so it cannot play online. What it needs beyond the default build:

- `wii/tools/data_layout.py` lays the game's data out as the GameCube's DOL has it (the sources
  rely on its order), and `extract_incs.py` extracts the byte tables the sources include from
  your `orig/GALE01/sys/main.dol`.
- `wii/tools/strip_asm.py` copies the SDK's sources without their MWCC assembly (both builds use
  it); `wii/src/mtx_ps.c` and the C in the SDK's own files stand in.
- `wii/include` has the headers GCC needs: the CPU's instructions MWCC has as intrinsics
  (`melee_wii/gekko.h`), MSL's `math.h` inlines, the game's `bool`.
