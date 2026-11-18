# Quick start

Melee for the Wii runs Super Smash Bros. Melee natively on the Wii, with Slippi Online and 20XX,
from your own disc image. No game files are included.

## What you need

- A Wii with the [Homebrew Channel](https://wiibrew.org/wiki/Homebrew_Channel) and an SD card
  (FAT32).
- Your own NTSC 1.02 disc image (`GALE01`, revision 2): a plain image ending in `.iso` (rename a
  `.gcm`; CISO and NKit images are not read).
- A GameCube controller, or a Wii Remote with a Classic Controller.
- For Slippi Online: an internet connection set up in Wii Settings, and a Slippi account.

## Setting up

1. Extract the release zip onto the SD card.
2. Add your NTSC 1.02 image as `/apps/melee/game.iso`.
3. For Slippi Online, put your Slippi account on the card:
   - Log in once with [Slippi Launcher](https://slippi.gg) on a PC. That saves your account as a
     file, `user.json`.
   - **Windows:** with the SD card in that PC, double-click `Copy my Slippi account (Melee).bat`
     at the top of the card. It copies the account Slippi Launcher is logged in to.
   - **macOS, Linux, or by hand:** copy `user.json` from Slippi Launcher's
     `netplay/User/Slippi/` folder to `/apps/melee/slippi/user.json` on the card. Slippi
     Launcher's folder is `%APPDATA%\Slippi Launcher` on Windows,
     `~/Library/Application Support/Slippi Launcher` on macOS and `~/.config/Slippi Launcher` on
     Linux.

   `user.json` is your Slippi login: keep it private.
4. For 20XX (optional), make a 20XX Hack Pack 5.0.2 image from your 1.02 image with
   [20XX's own creator](https://github.com/DRGN-DRC/20XX-HACK-PACK) and add it as
   `/apps/melee/20xx.iso`. The launcher then lists **20XX**.

   **Extra menu (optional):** with that image on the card, turn on **20XX menu** in the
   launcher's Settings. Slippi Online's Online Play screen then has an **Extra** button: 20XX's
   music playlists, stage skins, shield colours and costumes for your online games. They only
   change what your own console shows and plays, so matches stay in sync. It needs a 5.0.2
   image; other versions are untested.
5. Start **Melee for the Wii** from the Homebrew Channel.

The first start of Slippi Online makes Slippi's menus from your disc, which takes about a minute.
In the launcher, **Y** (y on a Classic Controller) says what each item still needs, and
**Game info** checks your disc image and account.

## Updates

From 1.1.0 on, the launcher looks for a newer version as it opens and asks before installing it
(Settings > Check for updates). Your settings, saves and account stay.

## Back to the Homebrew Channel

In the launcher, choose **Exit** (or press HOME on a Wii Remote). During a game, hold **HOME** on
a Wii Remote or its Classic Controller for a second, or press the console's **RESET** button.

## More

Settings, controllers, Slippi Online's input delay, 20XX, cheats and what to do when something
goes wrong: the [README](README.md).
