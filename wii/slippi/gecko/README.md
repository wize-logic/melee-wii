# Slippi's Gecko codesets

Codesets built from [slippi-ssbm-asm](https://github.com/project-slippi/slippi-ssbm-asm) (GPL-3.0),
as Slippi Nintendont ships them. `wii/src/slippi/gecko_blobs.S` links them into the program, and
`wii/src/slippi/so_wii.c` applies them before the game starts (`wii/src/gecko.c`).

| File | Applied |
|---|---|
| `g_online_boot.bin` | Slippi Online: the client's boot codes. Slippi's bootloader (which fetches the online codeset from the EXI device), the rumble button and the connection's label on the character select screen, no Ranked |
| `g_core.bin`, `g_crash_output.bin` | Offline, with Replays or Console mirroring on |
| `g_ucf_084*.bin`, `g_pal.bin`, `g_mods_*.bin`, `g_stages_*.bin`, `g_gameplay_*.bin`, `g_screen_wide*.bin`, `g_safety.bin` | Offline, by the launcher's settings |
| `g_core_porta.bin`, `g_lag_pd*.bin`, `g_ucf.bin`, `g_ucf_stealth.bin` | Not applied |

The online codeset itself is not here: it is `netplay.gct` on the SD card, from the release zip
(`wii/tools/slippi_files.py`).
