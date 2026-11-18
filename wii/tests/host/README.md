# Host tests for the online client's network code

The TLS, WebSocket, matchmaking, update and pad code of `wii/src/slippi` and
`wii/src/launcher/update.c`, built for Linux against small stand-ins for the
Wii's platform (`so_platform_host.c`, `so_tcp_posix.c`, `fake/`), and run
against the real services. `make` builds them in `build/`.

| Program | What it checks |
|---|---|
| `ws_probe` | TLS (certificate, host name) and the WebSocket upgrade to `matchmaking.slippi.gg`; a join with a made-up account gets the service's error |
| `stun_probe` | STUN binding requests to the four public servers, and their answers parsed |
| `mmws_probe` | the whole matchmaking session thread (`so_mmws.c`) with a made-up account |
| `https_probe [url [file]]` | an HTTPS GET with redirects (GitHub's API, a release download) |
| `pads_test` | 9-byte pads and the player drop helpers (`so_pads.c`) |
| `update_probe` | the launcher's update check and install, against the real releases, into a scratch folder |

None of them sends a real account. `update_probe` installs the latest
release into `build/sd/` (it pretends to be version 0.9.0).
