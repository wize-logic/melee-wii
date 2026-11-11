/* slippi_online/so_link.c
 * The Wii's connection type (see so_link.h).
 */
#include "so_link.h"
#include "so_alloc.h"
#include "so_platform.h"

#define HEADER_SIZE 8
#define CONN_TYPE_OFFSET 4	/* u32 version, then u8 connType */
#define SLOT_SIZE 0x91C
#define SLOTS 3

#define FLAG_WIRED 0x01
#define FLAG_SELECTED 0x80

#define CONN_TYPE_WIFI 1
#define CONN_TYPE_WIRED 2

SoLink so_link_parse(const u8 *data, u32 len)
{
	int i;
	for (i = 0; i < SLOTS; i++) {
		u32 at = HEADER_SIZE + (u32)i * SLOT_SIZE;
		if (at >= len)
			break;
		if (data[at] & FLAG_SELECTED)
			return data[at] & FLAG_WIRED ? SO_LINK_WIRED : SO_LINK_WIFI;
	}
	if (len > CONN_TYPE_OFFSET) {
		if (data[CONN_TYPE_OFFSET] == CONN_TYPE_WIFI)
			return SO_LINK_WIFI;
		if (data[CONN_TYPE_OFFSET] == CONN_TYPE_WIRED)
			return SO_LINK_WIRED;
	}
	return SO_LINK_UNKNOWN;
}

SoLink so_link_detect(void)
{
	/* Only the slots' first bytes matter: read up to the last one. (A buffer
	 * of its own: the launcher asks before so_alloc has its arena.) */
	static u8 buf[HEADER_SIZE + (SLOTS - 1) * SLOT_SIZE + 1];
	SoLink link = SO_LINK_UNKNOWN;
	int got = so_read_nand(SO_LINK_CONFIG_PATH, buf, sizeof(buf));
	if (got > 0)
		link = so_link_parse(buf, (u32)got);
	else
		so_log("link: could not read the network settings (%d)", got);
	return link;
}

const char *so_link_name(SoLink link)
{
	switch (link) {
	case SO_LINK_WIRED:
		return "wired";
	case SO_LINK_WIFI:
		return "Wi-Fi";
	default:
		return "unknown";
	}
}
