/* slippi_online/so_tcp.h
 * A TCP client connection for one thread of its own (so_tls.c, for the
 * matchmaking service and the launcher's updates). On the Wii these are
 * IOS socket calls: the connect blocks that thread (up to IOS's own
 * timeout), sends block until IOS takes the bytes, and receiving waits by
 * polling IOS with no timeout and sleeping in between, as console mirroring
 * does (so_console.c): no other socket's thread is ever held up behind an
 * IOS call of this one.
 */
#ifndef SO_TCP_H
#define SO_TCP_H

#include "global.h"

/* Returns the socket, or < 0. `addr` in network byte order. */
s32 so_tcp_connect(u32 addr, u16 port);
/* Sends everything; returns len, or < 0. */
s32 so_tcp_send(s32 sock, const void *data, u32 len);
/* Waits up to `timeout_ms` for data; returns the bytes read (> 0), 0 when
 * none came in time, or < 0 when the connection is closed or failed. */
s32 so_tcp_recv(s32 sock, void *buf, u32 size, u32 timeout_ms);
void so_tcp_close(s32 sock);

#endif
