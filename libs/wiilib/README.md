# wiilib

The small library the Wii port (`cmake/Wii.cmake`, `wii/src`) is built on, over libogc and libfat.

| Module | CMake target | What the port uses it for |
|---|---|---|
| `wl/base` | `wiilib::base` | Result codes, asserts, logging, big-endian reads |
| `wl/gcm` | `wiilib::gcm` | Reading the game's files out of a GameCube disc image: the header, the DOL, the file table. The image is untrusted: every offset, size and name is checked |
| `wl/platform` | `wiilib::platform` | SD and USB files (libfat; IOS 58 for USB 2.0), the log (Dolphin's OSREPORT and `log.txt` on the SD card), the reset and power buttons, leaving for the loader, the stop screen |

There is no video, GX, audio or input layer: the port brings its own (the SDK's, `wii/src`).

Wii only (devkitPPC, libogc). MIT licensed (`LICENSE`).
