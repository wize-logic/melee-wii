# Melee for the Wii

Super Smash Bros. Melee running natively on the Wii as a Homebrew Channel program, from your own
disc image, with a launcher and Slippi Online.

- **The game** is the original game code: the decompilation's matching build, placed at the
  GameCube's addresses, so it behaves exactly as on a GameCube and GameCube Gecko codes work. The
  parts of the GameCube's SDK a Wii can't run (OS, VI, PAD, DVD, EXI, the memory card, ARAM, AX/DSP
  sound, THP) are replaced by the port's own, over libogc.
- **The launcher** comes up first: Melee, Slippi Online or a mod, with settings and help.
- **Slippi Online** is a Slippi client on the Wii itself, with rollback, against players on Slippi
  Dolphin, over a LAN adapter or Wi-Fi.

Unofficial: not made by, affiliated with or endorsed by Nintendo or Project Slippi. No game files
are in this repository or in a release.

**Contents:** [What works](#what-works) · [What you need](#what-you-need) ·
[Installing](#installing) · [The launcher](#the-launcher) · [Slippi Online](#slippi-online) ·
[20XX](#20xx) · [Controllers](#controllers) · [Cheats](#cheats) ·
[Files on the SD card](#files-on-the-sd-card) ·
[When something goes wrong](#when-something-goes-wrong) · [Building](#building) ·
[The source](#the-source) · [Credits and licences](#credits-and-licences)

## What works

| | |
|---|---|
| The game | All modes, sound and music, movies, saves (on the SD card), 480p with a component cable |
| Slippi Online | Direct and Unranked, with rollback, over a wired connection or Wi-Fi; quick chat, connect code history, rumble |
| Replays | Every game, online and offline VS, as a `.slp` file |
| Console mirroring | Slippi Launcher can connect to the Wii to watch, record or relay games |
| Slippi's offline codes | UCF 0.84, PAL balance, Stealth/Tournament/Friendlies, frozen stages, ledge grab and wobbling limits, widescreen, safety |
| 20XX | 20XX Hack Pack (or another 1.02 mod) from its own disc image; part of its menu in the plain game and Slippi Online |
| Cheats | Your own Gecko codes, offline |
| Controllers | GameCube controllers; Classic Controllers on Wiis without GameCube ports |

Not there: Ranked (left out), Teams (reachable, untested), a delay-based netcode (rollback only),
and any disc other than NTSC 1.02.

## What you need

- A Wii with the [Homebrew Channel](https://wiibrew.org/wiki/Homebrew_Channel) and an SD card
  (FAT32). Settings, saves, logs and Slippi's files are always on the SD card.
- Your own NTSC 1.02 disc image (`GALE01`, revision 2): a plain, uncompressed image with a name
  ending in `.iso` (rename a `.gcm`; CISO and NKit images are not read). The launcher's **Game
  info** checks it: a 1:1 copy has the MD5 `0e63d4223b01d9aba596259dc155a174`.
- A GameCube controller, or a Wii Remote with a Classic Controller.
- For Slippi Online: an internet connection set up in Wii Settings, and a Slippi account (log in
  once with [Slippi Launcher](https://slippi.gg) on a PC).

## Installing

1. Extract the release zip onto the SD card. It adds `apps/melee` and two files beside it. (To
   make the zip: [Building](#building).)
2. Put your disc image on the card as `/apps/melee/game.iso`.
3. For Slippi Online, copy your account to `/apps/melee/slippi/user.json`:
   - on Windows, double-click `Copy my Slippi account (Melee).bat` on the SD card;
   - or by hand, from Slippi Launcher's `netplay/User/Slippi/user.json` in
     `%APPDATA%\Slippi Launcher` (Windows), `~/Library/Application Support/Slippi Launcher`
     (macOS) or `~/.config/Slippi Launcher` (Linux).

   `user.json` is your login: keep it private.
4. Start **Melee for the Wii** from the Homebrew Channel.

The disc image is looked for as `/apps/melee/game.iso`, `/apps/melee/GALE01.iso`,
`/games/GALE01.iso`, `/games/melee.iso` and `/melee.iso`, on the SD card and then on a USB drive.

## The launcher

| Item | |
|---|---|
| **Melee** | The game, with the codes chosen in Settings |
| **Slippi Online** | Shown once there is a disc image, an account and Slippi's files. The first start makes Slippi's menus from your disc (about a minute) |
| **20XX** / **Modded Melee** | Shown with a mod's disc image; Left/Right picks among several |
| **Settings**, **Game info**, **Exit** | Game info shows the disc, its MD5 check, the account and Slippi's files |

| | GameCube controller | Wii Remote (sideways, or its pointer) | Classic Controller |
|---|---|---|---|
| Move | D-pad, stick | D-pad | D-pad |
| Choose / back | A or Start / B | 2 or A / 1 or B | a / b |
| Settings / Game info | X / Z | + / − | + / − |
| Help: what each item needs | Y | | y |
| Leave | the Exit item | HOME | HOME |

### Settings

Kept in `sd:/apps/melee/settings.ini`.

| Setting | Choices (default first) | |
|---|---|---|
| Rumble | On, Off | Online. Also the rumble button on the online character select screen |
| Delay frames | Auto, 1–9 | Online: see [the input delay](#the-input-delay) |
| Controller fix | Off, UCF 0.84, UCF 0.84 (stealth) | Offline |
| PAL balance | Off, On | Offline |
| Convenience | Off, Stealth, Tournament, Friendlies | Offline: Slippi's convenience codes |
| Frozen stages | Off, Pokémon Stadium, All | Offline |
| Gameplay | Off, Ledge grab limit, Wobbling limit, Both | Offline |
| Screen | Auto, 4:3, Widescreen 16:9, 16:9 with 4:3 inside | Slippi's widescreen code; Auto follows Wii Settings |
| Safety | Off, On | Offline |
| Cheats | Off, On | Offline: see [Cheats](#cheats) |
| Replays | On, Off | |
| Console mirroring | On, Off | |
| Skip this screen | Off, On | Starts the last choice at once. Hold B as it starts to get the launcher back: on a GameCube controller, or with none plugged in, 1 or B on a Wii Remote (b on its Classic Controller) |
| Diagnostics | Off, On | Records each online game's timing: [docs/netplay.md](docs/netplay.md#diagnostics) |
| 20XX menu | Off, On | Shown with a 20XX image: see [20XX](#20xx) |

Offline, Replays and Console mirroring bring Slippi's core codes, which record the games and start
at the VS character select screen, as Slippi Nintendont does.

With a component cable and the Wii set to 480p, the game boots in 480p without asking; B held at
the start still asks.

## Slippi Online

Direct and Unranked against players on Slippi Dolphin, with Slippi's own rollback code and the
port's savestates. How it works, and what was measured:
[docs/netplay.md](docs/netplay.md).

- **Wired plays best:** a USB LAN adapter with the ASIX AX88772 chip (Nintendo's Wii LAN Adapter,
  or a third-party one with that chip). The connection is the one chosen in Wii Settings.
- **On Wi-Fi** the port turns off the Wi-Fi driver's roaming scan, which stopped the link for some
  150 ms about every 10 seconds, and pauses WiiConnect24. If roaming can't be turned off, the game
  says so and returns to the Homebrew Channel. A Wii without a cable to the router can still play
  wired through a Wi-Fi extender or travel router with the adapter plugged into it.
- **The character select screen** shows the connection (LAN or WiFi); a game shows the ping in its
  corner.
- **Replays** go to `/apps/melee/slippi/replays`, written as the game runs.
- **Console mirroring:** in Slippi Launcher, add the Wii under *Console Mirroring* (TCP 51441; the
  Wii announces itself on the local network).

### The input delay

A Wii replays fewer frames in a frame than a PC, so **Auto** picks the delay for each opponent
from the connection and the ping, as the match starts:

| Ping | Wired | Wi-Fi |
|---|---|---|
| up to 50 ms | 2 frames | 3 |
| up to 100 ms | 3 | 4 |
| over | 4 | 5 |

**Delay frames** in Settings fixes it at 1 to 9 instead. More settings, by hand, in
`/apps/melee/slippi/config.ini`: [docs/netplay.md](docs/netplay.md#configini).

## 20XX

Make your 20XX image with [20XX Hack Pack](https://github.com/DRGN-DRC/20XX-HACK-PACK)'s own
creator, from your NTSC 1.02 image, and put it in `/games`, `/apps/melee` or the root of the SD
card or a USB drive (any name ending in `.iso` or `.gcm`). The launcher lists it as **20XX**: it starts without Slippi and
keeps its own saves. Any other 1.02 mod is listed as **Modded Melee**. Up to four are listed.

20XX's own widescreen follows the launcher's Screen setting, and its lag reduction ("Are you
using HDMI?") works. Tested with 20XX 5.0.2.

### 20XX's menu in Melee and Slippi Online

To turn it on: with a 20XX image on the card, open the launcher's **Settings** (X; + on a Wii
Remote) and set **20XX menu** to On. It is off by default, and listed only while a 20XX image is
found.

It gives the part of 20XX's debug menu that changes only sound or picture, with your image's songs
and files:

- **Slippi Online:** **Extra** on the Online Play screen, under Party.
- **Melee:** **Y** on the main menu's first screen (Y opens it from the Online Play screen too).

The stick or D-pad moves, A opens or plays, B goes back, Start leaves.

| | |
|---|---|
| Music | 20XX's playlists (a global one, the menu's, and one for each of nine stages) with any of the image's songs; music off; the stages' second songs off |
| Stage textures | 20XX's looks for Battlefield, Final Destination, Yoshi's Story, Fountain of Dreams, Dream Land and Pokémon Stadium, or a random one each match |
| Stage colors | Final Destination's custom colour |
| Shield colors | Each player's shield colour. Online, the first is yours |
| 20XX costumes | L and R on the character select screen give your character its costume's two others. Online, only your own character wears one |

Nothing else of 20XX's menu is there: the rest changes the game, and an opponent on plain Slippi
must be playing the same match. For the same reason none of 20XX's code runs, and a stage look or
costume is only used if its file is the original with another look: the first start with the
setting on checks the image's files (about 40 s). For 20XX 5.0.2, 49 of 75 stage looks and 242 of
254 costumes pass. The whole of it: [docs/20xx.md](docs/20xx.md).

## Controllers

- **GameCube controllers**, with rumble.
- **A Wii Remote with a Classic Controller** (or Classic Controller Pro) plays as the GameCube
  controller of its number when none is plugged in there: + is Start, ZR is Z, ZL a full L press,
  the right stick the C-stick. No rumble. A Wii Remote alone is for the launcher only.

## Cheats

Your own Gecko codes for NTSC 1.02, offline, with Settings' **Cheats** on:
`/apps/melee/codes/GALE01.gct` (or `/codes/GALE01.gct`, there or on a USB drive), the file
Nintendont and Dolphin use, under 512 KB.

- Read: RAM writes and fills (00, 02, 04, 06, 08), if codes (20–2E), their pointer forms, base
  address and end-if codes (42, 4A, E0, E2), and inserted code (C2).
- Left out: C0 codes, and codes that patch the SDK functions the port replaces. A code type that
  isn't read ends the list; the log says which.

## Files on the SD card

All under `sd:/apps/melee/`.

| Path | What |
|---|---|
| `boot.dol`, `meta.xml`, `icon.png` | The program |
| `game.iso` | Your disc image |
| `settings.ini` | The launcher's settings |
| `card/` | Saves, one `.gci` each (memory card slot A). A mod's are in `card20xx/` or `cardmod/` |
| `codes/GALE01.gct` | Your cheats |
| `slippi/user.json` | Your Slippi account. Keep it private |
| `slippi/netplay.gct`, `slippi/files/`, `slippi/patches/` | Slippi's online code, menu files and patches, from the release zip. The launcher makes the patched menus from your disc |
| `slippi/replays/` | Replays |
| `slippi/config.ini`, `slippi/rumble.txt` | The online client's settings |
| `20xx.ini`, `20xx-stages.txt` | The 20XX menu's values, and the file check's verdicts (delete it to check again) |
| `logs/log.txt` | The log; the four runs before it as `log-1.txt` to `log-4.txt` |
| `logs/diag/` | With Diagnostics on, a record of each online game |

## When something goes wrong

- **An item or setting is missing in the launcher:** Help (Y) lists what each one needs and
  whether it is there.
- **A crash** is shown on the screen and written to `logs/log.txt`: where, the registers, the
  calls. A game that stops (no new frame for 5 s) is logged with what it waits in.
- **Skip this screen is on and the launcher won't come back:** hold B from before the program
  starts, or set `skip=0` in `settings.ini`.
- **Online games run badly:** turn Diagnostics on, play, and read the records with
  `wii/tools/gamelog.py` ([docs/netplay.md](docs/netplay.md#diagnostics)).

## Building

You need:

- devkitPro's `wii-dev` packages: devkitPPC (GCC 12 or newer), libogc 3.1 or newer, libfat;
- CMake 3.25 or newer, Ninja, Python 3.9 or newer;
- the decompilation's own matching build, which needs your `main.dol`: see
  [the main README](../.github/README.md). `ninja` makes `build/GALE01/main.dol` and `main.elf`.

```sh
python configure.py && ninja      # the matching build
cmake --preset wii
cmake --build --preset wii        # build/wii/melee.dol (and melee.elf, melee.map)
```

The preset expects devkitPro in `/opt/devkitpro`. Elsewhere:
`cmake -S . -B build/wii -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake`.

| Option | Default | |
|---|---|---|
| `MELEE_ORIG_CODE` | `ON` | The game as the matching build links it. `OFF` builds the game's C sources with GCC instead: no launcher, Slippi Online or Gecko codes, and its floating point differs from the GameCube's |
| `MELEE_DECOMP_ROOT` | this checkout | Takes the matching build from another checkout |

The release zip holds the SD card's files, with Slippi's files downloaded from Project Slippi's
GitHub (pinned and checked by SHA-256 in `wii/tools/slippi_files.py`). Your disc image only gives
the patched files' checksums; nothing of it goes in.

```sh
python3 wii/tools/make_release.py --iso Melee.iso      # build/release/melee-wii-v1.0.0.zip
```

The version is `MELEE_WII_VERSION` in `wii/src/launcher.h`.

## The source

| | |
|---|---|
| `cmake/Wii.cmake` | The build: the SDK and port libraries, Slippi, the DOL |
| `wii/src` | The SDK over libogc, the start (`main.c`, `boot_mem2.S`), Gecko codes, 20XX (`mod_*.c`) |
| `wii/src/launcher`, `wii/src/launcher*.c` | The launcher |
| `wii/src/slippi` | The Slippi client, with ENet (`enet/`) |
| `wii/slippi/gecko` | Slippi's Gecko codesets, built |
| `wii/include` | Headers for building the game's and the SDK's sources with GCC |
| `wii/tools` | The build's tools, the release's, and the PC side of the tests |
| `wii/release` | The release zip's own files |
| `libs/wiilib` | Files, the log, the system buttons, the GameCube disc reader |

- [docs/internals.md](docs/internals.md): how the port works, the memory layout, what a Wii does
  that Dolphin doesn't, testing.
- [docs/netplay.md](docs/netplay.md): the client, rollback, the input delay, Wi-Fi, Diagnostics.
- [docs/20xx.md](docs/20xx.md): 20XX and its menu.

## Credits and licences

- The game's decompilation: [doldecomp/melee](https://github.com/doldecomp/melee) and its
  contributors.
- [Project Slippi](https://github.com/project-slippi): Slippi, its online code and files. The
  client in `wii/src/slippi` is GPL (`wii/src/slippi/LICENSE`).
- [SlippWii](https://github.com/2gifts/SlippWii) by 2gifts: Slippi Online on a real Wii.
  [Slippi Nintendont](https://github.com/project-slippi/Nintendont): the console connection's
  protocol.
- [ENet](http://enet.bespin.org/) by Lee Salzman (MIT, `wii/src/slippi/enet`).
- [20XX Hack Pack](https://github.com/DRGN-DRC/20XX-HACK-PACK). Nothing of it is in this
  repository.
- [DejaVu fonts](https://dejavu-fonts.github.io/) (`wii/src/launcher/DejaVu-LICENSE.txt`); MD5 by
  L. Peter Deutsch (`wii/src/launcher/md5.c`).
- `libs/wiilib`: MIT (`libs/wiilib/LICENSE`).
