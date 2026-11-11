# ENet 1.3.13 (vendored)

Upstream: <https://github.com/lsalzman/enet>, tag `v1.3.13`, the version Slippi Dolphin
(Ishiiruka) bundles for matchmaking and netplay. MIT licence: see `LICENSE`.

| File | Status |
|---|---|
| `callbacks.c`, `host.c`, `list.c`, `packet.c`, `peer.c` | Upstream, unmodified |
| `include/enet/{callbacks,list,protocol,time,types,utility}.h` | Upstream, unmodified |
| `include/enet/enet.h` | Upstream, plus one change: it includes `enet/ios.h` when `ENET_IOS` is defined |
| `protocol.c` | Upstream, plus one change under `ENET_IOS`: each round trip measured from an acknowledgement is passed to `enet_ios_rtt_hook`, before ENet's smoothing |
| `include/enet/ios.h`, `ios.c` | New: the IOS platform layer (in place of `unix.c` and `win32.c`) |

`compress.c` (the range coder) is left out: Slippi does not use it. Line endings are LF, as
upstream.

## The IOS platform layer

`ios.c` implements ENet's platform API on `../so_socket.h`.

- **One UDP socket.** `enet_socket_bind` opens it on the requested port (a random one in
  41000–50999 for port 0, as Slippi Dolphin picks). `enet_socket_destroy` leaves it open for the
  next host on the same port and only flushes unread datagrams; `../so_socket.h` says why.
- **Nothing blocks.** Sends are queued; when every send buffer is in flight, `enet_socket_send`
  reports "would block" and ENet's reliability layer resends as usual.
- **Waiting.** `enet_socket_wait` and `enet_socketset_select` wait on the socket's wake event. The
  Slippi client services its host with a zero timeout and does its own waiting, because that event
  also carries wake-ups from the game.
- **Start-up.** Call `enet_ios_initialize` instead of `enet_initialize`: it routes ENet's memory to
  the client's arena and makes running out of memory non-fatal.
- **The round trip.** `enet_ios_rtt_hook`, when set, gets each raw round trip in milliseconds.
  ENet's own mean starts at 500 ms and takes dozens of readings to settle; the client keeps the
  newest 16 to choose a match's input delay (`../so_netplay.c`).
