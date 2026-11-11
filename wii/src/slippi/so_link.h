/* slippi_online/so_link.h
 * Which connection the Wii uses: wired (USB LAN adapter) or Wi-Fi.
 *
 * The answer chooses the network profile (so_client.c): Wi-Fi's IOS driver
 * sends far fewer datagrams per second than the LAN adapter's (FEASIBILITY.md,
 * M1b/M1c), so on Wi-Fi the client spends fewer datagrams on the same pads.
 *
 * It comes from the Wii's network settings, /shared2/sys/net/02/config.dat
 * (the layout Dolphin's WiiNetConfig.h documents): a header, then three
 * connection slots of 0x91C bytes, each starting with a flags byte where
 * 0x80 marks the selected connection and 0x01 a wired one. The header's
 * connType (1 Wi-Fi, 2 wired) answers when no slot is marked.
 */
#ifndef SO_LINK_H
#define SO_LINK_H

#include "global.h"

typedef enum {
	SO_LINK_UNKNOWN = 0,
	SO_LINK_WIRED,
	SO_LINK_WIFI,
} SoLink;

#define SO_LINK_CONFIG_PATH "/shared2/sys/net/02/config.dat"
#define SO_LINK_CONFIG_SIZE 0x1B5C	/* 8 + 3 * 0x91C */

/* The connection type in a config.dat image (`len` bytes of it). */
SoLink so_link_parse(const u8 *data, u32 len);

/* Reads config.dat from NAND; SO_LINK_UNKNOWN when it cannot. */
SoLink so_link_detect(void);

const char *so_link_name(SoLink link);

#endif
