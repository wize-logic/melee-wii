#!/usr/bin/env python3
"""Makes the release zip of Melee for the Wii: the files to put on the SD card.

    cmake --build --preset wii                                   # build/wii/melee.dol
    python3 wii/tools/make_release.py --iso Melee.iso            # build/release/melee-wii-v1.0.0.zip

The zip mirrors the SD card (extract it onto the card):

    apps/melee/                         the app (Homebrew Channel): boot.dol, meta.xml, icon.png
    apps/melee/slippi/netplay.gct       Slippi's online code (as Slippi Dolphin runs it)
    apps/melee/slippi/files/            Slippi's own menu files
    apps/melee/slippi/patches/*.diff    Slippi's patches for Melee's menu files, applied on
                                        the Wii to the player's disc (the launcher,
                                        wii/src/launcher/files.c); nothing of Melee's is shipped
    apps/melee/slippi/patches/manifest.txt
                                        what each file must be (size, CRC-32): the patches
                                        carry no checksums of their own
    apps/melee/codes/                   for the player's own Gecko codes (GALE01.gct)
    Copy my Slippi account (Melee).bat  copies user.json from Slippi Launcher
                                        (apps/melee/slippi/copy-account.ps1)

Slippi's files come from Project Slippi's GitHub (pinned in
wii/tools/slippi_files.py). --iso is your own NTSC 1.02 disc: the manifest
needs the patched files' sizes and CRC-32s, and only those are kept. The
version is wii/src/launcher.h's MELEE_WII_VERSION.
"""
import argparse
import datetime
import hashlib
import re
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'wii/tools'))
import slippi_files  # noqa: E402

REL = ROOT / 'wii/release'

README = """Melee for the Wii {version}
========================

Super Smash Bros. Melee (NTSC 1.02), decompiled and running natively on the
Wii, with Slippi Online. Unofficial: NOT made by, affiliated with or endorsed by
Nintendo or Project Slippi.

You need: a Wii with the Homebrew Channel, your own NTSC 1.02 Melee disc image
(GALE01 revision 2, a plain .iso/.gcm), and for Slippi Online an internet
connection set up in Wii Settings and a Slippi account (log in once with Slippi
Launcher on your PC: https://slippi.gg).

1. Extract this zip onto your Wii's SD card (it adds apps/melee and this
   folder's two files; nothing else is changed).
2. Put your disc image on the card as /apps/melee/game.iso (or /games/melee.iso,
   on the SD card or a USB drive).
3. For Slippi Online: double-click "Copy my Slippi account (Melee).bat" on the
   SD card (it copies your login from Slippi Launcher), or copy user.json into
   /apps/melee/slippi by hand. SlippWii's account on the card is used by
   itself.
4. Start "Melee for the Wii" from the Homebrew Channel and pick Melee or
   Slippi Online. The first Slippi Online start makes Slippi's menus from your
   disc (a minute).

Saves go to /apps/melee/card, replays to /apps/melee/slippi/replays, your
cheats (offline) are /apps/melee/codes/GALE01.gct.
"""

NOTICE = """Melee for the Wii {version} - Slippi's files in this folder

netplay.gct, files/ and patches/*.diff are Project Slippi's, unchanged except
that netplay.gct leaves out Slippi Dolphin's music hooks and "Normal Lag
Reduction" (Dolphin-only), as a console needs:
  - netplay.gct: Slippi's online code (slippi-ssbm-asm, GPLv3), from
    https://github.com/project-slippi/Ishiiruka ({stable}, Data/Sys/GameSettings/GALE01r2.ini)
  - files/, patches/: Slippi Dolphin's game files and patches (GPLv2+), from
    https://github.com/project-slippi/dolphin ({release}, Data/Sys/GameFiles/GALE01/)
The launcher applies the patches on the Wii to your own Melee disc; no Melee
files are included.
"""

CODES = """Your own Gecko codes for Melee (NTSC 1.02), offline: put them here as
GALE01.gct (the same file Nintendont and Dolphin use), and turn on
Settings > Cheats in the launcher. They are not used in Slippi Online.
"""


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--iso', required=True, help="your NTSC 1.02 Melee disc (only the patched files' CRC-32s are kept)")
    ap.add_argument('--dol', default=str(ROOT / 'build/wii/melee.dol'))
    ap.add_argument('--out', default=str(ROOT / 'build/release'))
    args = ap.parse_args()

    m = re.search(r'#define MELEE_WII_VERSION "([^"]+)"', (ROOT / 'wii/src/launcher.h').read_text())
    v = m.group(1)
    dol = Path(args.dol)
    if not dol.is_file():
        sys.exit(f'{dol} is missing: build it first (cmake --build --preset wii)')

    meta = (REL / 'apps/melee/meta.xml').read_text(encoding='utf-8')
    meta = re.sub(r'<version>[^<]*</version>', f'<version>{v}</version>', meta)
    meta = meta.replace('<release_date></release_date>',
                        f'<release_date>{datetime.date.today():%Y%m%d}000000</release_date>')

    # Slippi's files, and what the patched ones must come out as.
    ini = slippi_files.fetch(slippi_files.CODESET[0], slippi_files.CODESET[1],
                             slippi_files.CACHE / 'GALE01r2-stable.ini', slippi_files.STABLE_RAW).decode('utf-8')
    gct, _, _ = slippi_files.build_gct(ini)
    disc = slippi_files.open_disc(args.iso)
    slippi = 'apps/melee/slippi'
    shipped, manifest = {}, [f'# Melee for the Wii {v}: what each file must be (kind, path in {slippi}/, size, CRC-32).',
                             '# "patch": made on the Wii from the disc with patches/<name>.diff; "copy": shipped.']
    manifest.append(f'copy netplay.gct {len(gct)} {zlib.crc32(gct):08x}')
    shipped[f'{slippi}/netplay.gct'] = gct
    for name, sha in slippi_files.GAME_FILES.items():
        data = slippi_files.fetch(f'GameFiles/GALE01/{name}', sha, slippi_files.CACHE / 'GameFiles' / name)
        if name.endswith('.diff'):
            target = name[:-5]
            made = slippi_files.decode(disc.file(target), data)
            manifest.append(f'patch files/{target} {len(made)} {zlib.crc32(made):08x}')
            shipped[f'{slippi}/patches/{name}'] = data
        else:
            manifest.append(f'copy files/{name} {len(data)} {zlib.crc32(data):08x}')
            shipped[f'{slippi}/files/{name}'] = data
    shipped[f'{slippi}/patches/manifest.txt'] = ('\n'.join(manifest) + '\n').encode()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f'melee-wii-v{v}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('apps/melee/boot.dol', dol.read_bytes())
        z.writestr('apps/melee/meta.xml', meta)
        z.writestr('apps/melee/icon.png', (REL / 'apps/melee/icon.png').read_bytes())
        z.writestr('apps/melee/codes/README.txt', crlf(CODES))
        for path, data in shipped.items():
            z.writestr(path, data)
        for name in ('copy-account.ps1', 'PUT user.json HERE.txt'):
            z.writestr(f'{slippi}/{name}', crlf((REL / slippi / name).read_text(encoding='utf-8')))
        z.writestr(f'{slippi}/NOTICE.txt', crlf(NOTICE.format(version=v, stable=slippi_files.STABLE_RAW.split('/')[-3],
                                                                release=slippi_files.RELEASE)))
        bat = 'Copy my Slippi account (Melee).bat'
        z.writestr(bat, crlf((REL / bat).read_text(encoding='utf-8')))
        z.writestr('Melee for the Wii README.txt', crlf(README.format(version=v)))
    rev = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    print(f'{out} ({out.stat().st_size // 1024} KB)\n'
          f'  zip sha256 {hashlib.sha256(out.read_bytes()).hexdigest()}\n'
          f'  boot.dol sha256 {hashlib.sha256(dol.read_bytes()).hexdigest()}\n'
          f'  source: {rev}')


if __name__ == '__main__':
    main()
