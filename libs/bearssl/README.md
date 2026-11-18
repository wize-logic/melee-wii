# BearSSL 0.6

[BearSSL](https://bearssl.org/) 0.6 by Thomas Pornin (MIT licence, `LICENSE.txt`), unchanged:
`inc/` and `src/` from `bearssl-0.6.tar.gz` (SHA-256
`6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`).

The Wii's TLS client for Slippi Online's matchmaking and the launcher's updates
(`wii/src/slippi/so_tls.c`). The build (`cmake/Wii.cmake`) turns off the parts that need an
operating system (time, `/dev/urandom`) or another CPU; the port gives it the time and its seed.
