# Slippi Online on the Wii

How the port plays Slippi Online against Slippi Dolphin: the client, rollback, the input delay,
Wi-Fi, replays, and the Diagnostics record. For players: [the README](../README.md#slippi-online).

## The client

`wii/src/slippi` is a Slippi online client written for Slippi Nintendont's kernel, here on the
PowerPC beside the game, over libogc (`so_platform_ogc.c`, `so_socket_ogc.c`). It follows Slippi
Dolphin (Ishiiruka) 3.6.4, and reports itself as that version.

| Part | Files |
|---|---|
| Slippi's EXI device, which the online codeset in the game talks to | `so_exi.c` (through `wii/src/exi.c`, slot B) |
| Matchmaking | `so_mm.c`, `so_mm_msg.c` |
| The connection to the opponent | `so_netplay.c`, `so_pads.c`, `enet/` (ENet 1.3.13) |
| Rollback | `so_rollback.c`, `so_rollback_ranges.c`, `wii/src/mmu_track.c`, `wii/src/copy.c` |
| Settings and the account | `so_config.c` (`config.ini`, `user.json`) |
| Slippi's game files and codeset | `so_files.c`, `gecko_blobs.S` (`wii/slippi/gecko`) |
| Replays, console mirroring | `so_replay.c`, `so_console.c` |
| Diagnostics | `so_gamelog.c` |
| The start, before the game's `main` | `so_wii.c`, `so_client.c` |

Modes: Direct and Unranked. Teams is reachable but untested. Ranked is left out (hidden by the
boot codes, and refused by the client).

| Traffic | Where |
|---|---|
| Matchmaking | ENet over UDP to `mm.slippi.gg:43113` |
| The match | ENet over UDP, peer to peer, from the same local port (random in 41000–50999) |
| Console mirroring | TCP 51441 on the Wii; a `SLIP_READY` broadcast on UDP 20582 every 10 s |

## Rollback

Slippi's online code does the game's part of rollback: it predicts the opponent's late inputs,
asks for a savestate before each predicted frame, and when the real inputs differ, asks for the
state back and plays the frames again within one displayed frame. `so_rollback.c` does the
device's part, as Slippi Dolphin does. There is no delay-based mode: a game whose savestates can't
be set up ends, with the reason on the character select screen.

### What a savestate holds

What Slippi Dolphin's holds (7.5 MB): the game's data and bss less the sound and video drivers'
state, OSAlloc's heap table, the audio heap with the Seq heap and the start of the Stay heap, and
the match's heap. The port finds them from the game's own records.

- **Slippi's codeset is in it** (the region from 0x8065C000). The codes keep data of their own
  there; left out of a rollback, it had the Wii and the PC play the frames again from different
  values. On the Wii the GPU's command FIFO ends in that region; the savestates start above it.
- **Not in it,** unlike Slippi's: the game's disc and ARAM requests (HSD's devcom) and the music
  stream's counters, which run on the audio's time. On a PC Slippi plays the music; on the Wii the
  game streams it from the SD card during the match, and a request rolled back with the frames
  stopped the reads.

### Only what changed is copied

While a rollback game runs, MEM1 above 2 MB is mapped through the CPU's page table instead of a
block translation (`mmu_track.c`), so the CPU marks each 4 KB page it writes.

- The state is cut into 512-byte pieces, each with its copy in a buffer of MEM2 and that copy's
  64-bit hash.
- A capture hashes the pieces of the pages written since the last, and gives each piece whose hash
  differs a new buffer for its copy, keeping the old one as the capture's undo record. Then it
  clears the marks and the TLB, once.
- A load puts back the pieces the captures since its target recorded, and of the pages written
  since the last capture, the pieces whose hash is not their copy's.
- The hash rotates each word before it multiplies. One that only multiplied kept a change of a
  word's top bit in the top bit, so two floats changing sign together (a turn's facing direction
  and speed) could leave it unchanged, and the savestate kept the old values.
- Writes the CPU doesn't make are marked by the port (disc reads). The GPU's texture copies (the
  characters' shadows) are left out: drawn again each frame, read by nothing but the GPU.

Measured on a console: some 250 pages written and 1 MB hashed for a savestate, of which about
200 KB differs; 3.5 ms a savestate, bounded by the hashing (through the locked cache).

### The copies

`copy.c` copies by the CPU (cache lines, the next ones prefetched) or by the locked cache's DMA.
When the savestates' memory is first set up, each way is timed and checked byte for byte, and the
faster one that is right is kept. On a console the CPU reads MEM2 slowly (a load took 2.5 times a
savestate's time). A DMA queue that stalls (20 ms) has its copies done again by the CPU, and the
CPU does them from then on.

### The sound through a rollback

The game asks for savestates and loads with its interrupts off, 3 to 4 ms each and one after the
other in a frame that goes back, and the audio DMA wants its next 5 ms block set by an interrupt.

- The audio's interrupts are handled before and after each savestate and load
  (`melee_wii_audio_service`), so no block is lost.
- Only when a block begins inside one and it runs past that block's end is the next block silence
  instead of the same 5 ms again (`melee_wii_audio_guard`).
- The long copies at a game's start still set silence (`melee_wii_audio_hold`).

### Inputs and time

- **The opponent's inputs** go to the game 7 at a time from the oldest it hasn't finalized, as
  Slippi Dolphin sends them. (The newest 7 left frames out after a lag spike: a desync.)
- **The window:** the Wii plays ahead of the opponent by up to 7 frames, and waits a frame now and
  then when it runs ahead of the opponent's clock, as Slippi Dolphin does.
- **Slippi's input code runs once at a time.** A savestate or a load takes no time in Dolphin; on
  the console it takes milliseconds with interrupts off, and Slippi's codes had never run that
  way. Two things followed, each a desync, and `so_exi.c` guards against both (the log counts
  them):
  - The codes' input routine could run inside itself: the main loop asks for the input a rollback
    held back just as the retrace's timer, late, asks too, and two inputs went in for one frame
    counted. The second request is told to skip.
  - After a long rollback the game ran a frame behind the timer, with two pad reads waiting where
    the codes expect one. A read now waits, up to 3 polls, until the game has taken the last.
- **A checksum for the frame it is.** Slippi's game code sends a frame's number with the checksum
  of the frame before when it has none for its newest frame. The port hands the game such a
  checksum as the frame's it belongs to (`remote_sum`), or Slippi's desync detection ends a match
  whose two sides are the same.

### Slippi Dolphin's rounding

Slippi Dolphin does the single-precision fused multiply-adds (`fmadds`, `ps_madd`, ...) as a
double-precision FMA rounded to single: two roundings, where the Wii's FPU rounds once, so the
last bit differs where the double lands halfway between two singles. Its negated forms also give
+0 where the Wii gives -0. In a minute of play that is tens of thousands of results (bone
rotations, matrix inverses, `atanf`, `fmodf`), each a desync waiting for a path into the game's
state.

For online play `wii/src/fused.c` has each of these instructions, in the game's code and Slippi's
codeset, compute what Slippi Dolphin does instead: a `bl` to code shared by all sites of an
instruction where the link register is free, a branch to code of its own elsewhere, all of it in
the bodies of the SDK functions the port replaces and other unused MEM1. It tests itself at the
start (the hashes in the log).

Two files in `sd:/apps/melee` are for tests: `nofused` turns it off, and `fusedcount` counts the
results that differ (in Dolphin, with a 32 MB MEM1).

## The input delay

A Wii replays fewer frames in a frame than a PC, so the delay is chosen for each opponent by the
connection and the round trip to them (`so_config_auto_delay`):

| Round trip | Wired | Wi-Fi |
|---|---|---|
| up to 50 ms | 2 frames | 3 |
| up to 100 ms | 3 | 4 |
| over | 4 | 5 |

- The round trip is the median of ENet's newest 16 readings since connecting (a ping every 500 ms
  on the character select screen). With no reading yet, 75 ms is assumed.
- The game takes the delay as the match starts. The log's `game: delay ...` line and the
  Diagnostics record say what was measured and chosen.
- The launcher's **Delay frames** setting (`input_delay=` in `config.ini`) overrides it with a
  fixed 1 to 9.
- The character select screen shows the connection (LAN or WiFi) where the boot codes have a
  delay setting; a game shows the ping in the corner where Slippi showed the delay.

**`delay_adjust=1`** in `config.ini` (off without it; only with Auto) lets the delay follow the
ping during a game, up or down a frame at a time: when the median ping of the last 3 s has been
5 ms past the delay's limit for 3 s, and not within 10 s of the start or of the last change. Slippi
fixes the delay for a game, so a change rewrites the game's ring of delayed pads and what the
opponent is sent (`so_exi.c`, "the input delay changed during a game"): a longer delay repeats one
frame of the player's input, a shorter one drops one. It checks the game's data against the pad it
is answering before writing, and turns itself off for the session if anything is not as expected.
Untested against a PC opponent's desync check: the log's `game: delay N -> M ...` lines say when
it acted.

## The connection

Slippi Online plays over the connection chosen in Wii Settings, and the client's network settings
follow it (`so_config_apply_link`). Wired is a USB LAN adapter with the ASIX AX88772 chip:
Nintendo's Wii LAN Adapter, or a third-party one with that chip.

### Wi-Fi

`wii/src/wifi.c` does what the Revolution SDK's network libraries do, without them.

- **Roaming is turned off.** The Wi-Fi driver scans for a better access point about every 10 s
  while connected, and each scan stops the link for some 150 ms (a 5- to 7-frame rollback). In
  Slippi Online on Wi-Fi the port sets the driver's `roam_off` through `/dev/wl0` when the network
  comes up, and checks it again after every match (the driver turns roaming back on when it
  reconnects). If roaming can't be turned off, the game stops with a message and returns to the
  Homebrew Channel. Offline, and on a wired connection, the driver is left alone.
- **WiiConnect24** is paused while the game runs, as the SDK's `NWC24SuspendScheduler` does, and
  resumed as it leaves. The log says whether it was running.
- **Rates and retries** are left to the Wi-Fi module, as Wii Settings saves them. Two `config.ini`
  lines set them for one session, for tests: `wifi_rate=54` or `1`, and `wifi_retries=1` to `15`.
  The settings saved in NAND stay as they are.
- **The signal** is in the Diagnostics record every 250 ms (the last frame the Wi-Fi module
  received, as IOS keeps it at `0x800031A0`).
- One thread makes every socket call in turn on Wi-Fi (`so_socket.h` says why); on the LAN
  adapter a receive thread stays blocked in `recvfrom`.

## Replays and console mirroring

- **Replays** are what Slippi's recording codes send the device each game, written as the game
  runs to `sd:/apps/melee/slippi/replays/Game_<date>T<time>.slp` (a whole game doesn't fit in
  memory beside the savestates). The metadata holds the start time and `playedOn: "wii"` only. A
  card too slow for the stream cuts the replay short.
- **Console mirroring** is Slippi Nintendont's protocol: Slippi Launcher connects to the Wii to
  watch, record or relay games.
- **Melee's own games** are recorded and mirrored only with the launcher's **Record Melee's games**
  (`offline_record=`, off by default): it brings Slippi's core codes into Melee, which record the
  games, start at the VS character select screen and unlock everything, as Slippi Nintendont
  does. Without it Melee runs none of Slippi's code.

## Diagnostics

For finding what makes online games run badly on a console and its connection. With the launcher's
**Diagnostics** setting on, each online game is recorded in memory as it runs (nothing is written
or logged during the game, so the record changes nothing of what it measures) and saved after it
as `sd:/apps/melee/logs/diag/game-<date>T<time>.bin`, up to 1 MB a game; the newest 40 are kept.
The log gets a few `diag:` lines of summary for each game too.

The record has:

- **a line for each time the game asked for a frame's inputs:** when; whether it played the frame
  or waited (for the opponent's inputs, to stay in time with the opponent, or for the pad); how
  many frames ahead of the opponent's inputs it was; how many frames it played again and how long
  the savestates and the load took; how long it waited for the next retrace and whether the
  picture came a retrace late; the ping, and how far ahead of the opponent the game runs in time;
- **a line for each of the opponent's pad packets:** when it came and how many new frames it
  brought;
- **a line for each thing the console did beside the game** that can hold the network or the game:
  the console connection's broadcast, each read of the disc image and write of the replay, each
  line of the log, each datagram that took 5 ms or more to send or receive;
- **the game's totals:** frames, waits, packets sent, received and resent, the savestates' cost.

MEM2 has about 1 MB left beside the savestates and the game's ARAM, so the record is small (16
bytes a poll, 8 a packet) and is taken after the game has its own memory: all of it holds 10
minutes of a game, and with less room it holds less (the log says how much) or there is none.

`wii/tools/gamelog.py` reads the files:

| Option | Prints |
|---|---|
| (none) | A summary of each game: how long frames took, the game's work in a frame with and without a rollback, the packets' spacing, the worst seconds, the signal's range |
| `--stalls` | Each time the opponent's packets stopped (over 100 ms), with what the console was doing then |
| `--slow` | The late frames (over 21 ms), with what happened in each |
| `--signal` | Each Wi-Fi signal reading |
| `--csv`, `--packets`, `--events` | Everything |

## `config.ini`

`sd:/apps/melee/slippi/config.ini`, `key=value` lines (`#` or `;` starts a comment). The launcher
writes `input_delay`; the rest is by hand. `so_config.h` describes every key.

| Key | Values | Default |
|---|---|---|
| `input_delay` | `auto`, `1`–`9` | `auto` |
| `delay_adjust` | `0`, `1` | `0` |
| `chat`, `rumble` | `0`, `1` | `1` (`rumble.txt`, which the character select screen's rumble button writes, wins) |
| `wifi_rate` | `wii`, `54`, `1` | `wii` |
| `wifi_retries` | `wii`, `1`–`15` | `wii` |
| `net_io` | `auto`, `threads`, `poll` | `poll` on Wi-Fi, `threads` wired |
| `local_port` | a UDP port, `0` for a random one | `0` |
| `log_host` | `ip[:port]`: the log as UDP datagrams to a PC | none (port 51450) |

The `test_*` keys make lag, packet loss and slow savestates for tests (`test_rx_stall_ms` makes
the lag spikes the long rollbacks are tested with).
