/* slippi_online/so_mm_msg.c
 * Matchmaking messages (see so_mm.h): the join message, STUN, and reading
 * the match, following Slippi Dolphin 3.7.0's SlippiMatchmaking and its
 * Rust matchmaking crate (protocol.rs, stun.rs).
 */
#include "so_mm.h"
#include "so_json.h"
#include "so_util.h"
#include "string.h"

/* The connect code as the game wrote it: an array of byte values */
static void write_code(SoJsonWriter *w, const SoSearch *search)
{
	int i;
	so_jw_array_begin(w);
	for (i = 0; i < search->connect_code_len; i++)
		so_jw_long(w, search->connect_code[i]);
	so_jw_array_end(w);
}

static void write_candidate(SoJsonWriter *w, u32 addr, u16 port, const char *type)
{
	char ip[16];
	so_format_ipv4(addr, ip, sizeof(ip));
	so_jw_object_begin(w);
	so_jw_key(w, "ip");
	so_jw_string(w, ip);
	so_jw_key(w, "port");
	so_jw_long(w, port);
	so_jw_key(w, "type");
	so_jw_string(w, type);
	so_jw_object_end(w);
}

int so_mm_build_join(char *out, int size, const SoConfig *config, const SoUser *user, const SoSearch *search,
	u16 local_port, const char *lan_address, const SoStunResult *stun)
{
	SoJsonWriter w;
	u32 lan_ip;
	u16 lan_port;
	int have_lan = lan_address[0] && so_parse_endpoint(lan_address, &lan_ip, &lan_port) == 0;
	const char *nat = "unknown";
	if (stun->answered[0] && stun->answered[1])
		nat = stun->addr[0] == stun->addr[1] && stun->port[0] == stun->port[1] ? "cone" : "symmetric";
	/* (as protocol.rs's Join, in its order) */
	so_jw_init(&w, out, size);
	so_jw_object_begin(&w);
	so_jw_key(&w, "type");
	so_jw_string(&w, "join");
	so_jw_key(&w, "user");
	so_jw_object_begin(&w);
	so_jw_key(&w, "uid");
	so_jw_string(&w, user->uid);
	so_jw_key(&w, "playKey");
	so_jw_string(&w, user->play_key);
	so_jw_key(&w, "connectCode");
	so_jw_string(&w, user->connect_code);
	so_jw_key(&w, "displayName");
	so_jw_string(&w, user->display_name);
	so_jw_object_end(&w);
	so_jw_key(&w, "search");
	so_jw_object_begin(&w);
	so_jw_key(&w, "mode");
	so_jw_long(&w, search->mode);
	so_jw_key(&w, "connectCode");
	write_code(&w, search);
	so_jw_object_end(&w);
	so_jw_key(&w, "appVersion");
	so_jw_string(&w, config->app_version);
	/* the port the service should tell the others: the one STUN saw */
	so_jw_key(&w, "port");
	so_jw_long(&w, stun->answered[0] ? stun->port[0] : local_port);
	so_jw_key(&w, "ipAddressLan");
	so_jw_string(&w, lan_address);
	if (stun->answered[0] || have_lan) {
		so_jw_key(&w, "candidates");
		so_jw_array_begin(&w);
		if (stun->answered[0])
			write_candidate(&w, stun->addr[0], stun->port[0], "srflx");
		if (have_lan)
			write_candidate(&w, lan_ip, lan_port, "host");
		so_jw_array_end(&w);
	}
	so_jw_key(&w, "natType");
	so_jw_string(&w, nat);
	so_jw_object_end(&w);
	return so_jw_finish(&w);
}

/* ---- STUN (RFC 5389 binding; matchmaking/src/stun.rs) -------------------- */

#define STUN_COOKIE 0x2112A442u

void so_stun_request(u8 out[SO_STUN_REQUEST_LEN], const u8 txid[12])
{
	so_put_u16(out, 0x0001);	/* binding request */
	so_put_u16(out + 2, 0);	/* no attributes */
	so_put_u32(out + 4, STUN_COOKIE);
	memcpy(out + 8, txid, 12);
}

int so_stun_parse(const u8 *msg, u32 len, const u8 txid[12], u32 *addr, u16 *port)
{
	u32 off = 20, end;
	int found = 0;
	if (len < 20 || so_get_u16(msg) != 0x0101 || so_get_u32(msg + 4) != STUN_COOKIE ||
		memcmp(msg + 8, txid, 12) != 0)
		return -1;
	end = 20 + so_get_u16(msg + 2);
	if (end > len)
		end = len;
	while (off + 4 <= end) {
		u16 type = so_get_u16(msg + off), alen = so_get_u16(msg + off + 2);
		const u8 *v = msg + off + 4;
		if (off + 4 + alen > end)
			break;
		if ((type == 0x0020 || (type == 0x0001 && !found)) && alen >= 8 && v[1] == 0x01) {
			u16 p = so_get_u16(v + 2);
			u32 a = so_get_u32(v + 4);
			if (type == 0x0020) {
				p ^= (u16)(STUN_COOKIE >> 16);
				a ^= STUN_COOKIE;
			}
			*port = p;
			/* (network byte order is the Wii's own) */
			{
				u8 b[4];
				so_put_u32(b, a);
				memcpy(addr, b, 4);
			}
			found = type == 0x0020 ? 2 : 1;
			if (found == 2)
				break;
		}
		off += 4 + ((alen + 3u) & ~3u);
	}
	return found ? 0 : -2;
}

/* "a.b.c.d:port" -> the part before the colon. */
static void host_part(const char *address, char *out, int size)
{
	const char *colon = strchr(address, ':');
	int n = colon ? (int)(colon - address) : (int)strlen(address);
	if (n >= size)
		n = size - 1;
	memcpy(out, address, n);
	out[n] = '\0';
}

static void read_player(const SoJson *el, SoMmPlayer *p)
{
	SoJson chat, item;
	int i;
	memset(p, 0, sizeof(*p));
	p->is_local = (u8)so_json_get_bool(el, "isLocalPlayer", 0);
	so_json_get_string(el, "uid", p->uid, sizeof(p->uid), "");
	so_json_get_string(el, "displayName", p->display_name, sizeof(p->display_name), "");
	so_json_get_string(el, "connectCode", p->connect_code, sizeof(p->connect_code), "");
	p->port = (u8)so_json_get_long(el, "port", 0);
	p->is_bot = (u8)so_json_get_bool(el, "isBot", 0);
	/* Dolphin keeps a player's chat messages only when there are exactly 16. */
	if (so_json_get(el, "chatMessages", &chat) == 0 && so_json_count(&chat) == SO_CHAT_MESSAGES) {
		p->has_chat_messages = 1;
		for (i = 0; i < SO_CHAT_MESSAGES; i++) {
			so_json_at(&chat, i, &item);
			if (so_json_string(&item, p->chat_messages[i], SO_CHAT_TEXT_MAX) < 0)
				p->has_chat_messages = 0;
		}
	}
}

int so_mm_parse_match(const char *text, u32 len, SoMatchResult *result, char *error, int error_size)
{
	static const u16 default_stages[] = { 0x3, 0x8, 0x1C, 0x1F, 0x20 };
	SoJson doc, players, stages, el;
	char local_external[24] = "", address[32], host[24];
	int i, n, found_local = 0;

	memset(result, 0, sizeof(*result));
	/* (3.7.0's handleMatchmaking no longer checks the type: the session
	 * hands over only the match message) */
	if (so_json_parse(&doc, text, len) < 0 || so_json_type(&doc) != SO_JSON_OBJECT) {
		strlcpy(error, "Invalid response when getting mm status", error_size);
		return -1;
	}

	so_json_get_string(&doc, "matchId", result->match_id, sizeof(result->match_id), "");

	/* Players, then the local player's external IP. */
	if (so_json_get(&doc, "players", &players) == 0) {
		n = so_json_count(&players);
		if (n > SO_MAX_PLAYERS) {
			strlcpy(error, "Too many players in match", error_size);
			return -3;
		}
		for (i = 0; i < n; i++) {
			SoMmPlayer *p = &result->players[i];
			so_json_at(&players, i, &el);
			read_player(&el, p);
			if (p->is_local) {
				found_local = 1;
				so_json_get_string(&el, "ipAddress", address, sizeof(address), "1.1.1.1:123");
				host_part(address, local_external, sizeof(local_external));
				result->local_player_index = (u8)(p->port - 1);
			}
		}
		result->player_count = (u8)n;
		if (!found_local || result->local_player_index >= n) {
			strlcpy(error, "Invalid response when getting mm status", error_size);
			return -5;
		}

		/* Where to reach each remote player: its LAN address when we share
		 * an external IP (same LAN, VPN node or CGNAT), else its external
		 * address. */
		for (i = 0; i < n; i++) {
			SoMmPlayer *p = &result->players[i];
			char lan[32];
			if (p->port - 1 == result->local_player_index)
				continue;
			so_json_at(&players, i, &el);
			so_json_get_string(&el, "ipAddress", address, sizeof(address), "1.1.1.1:123");
			so_json_get_string(&el, "ipAddressLan", lan, sizeof(lan), "1.1.1.1:123");
			host_part(address, host, sizeof(host));
			if (strcmp(host, local_external) == 0 && lan[0])
				strlcpy(address, lan, sizeof(address));
			if (so_parse_endpoint(address, &p->addr, &p->udp_port) < 0) {
				strlcpy(error, "Invalid player address from mm server", error_size);
				return -4;
			}
		}
	}
	if (result->player_count < 2) {
		strlcpy(error, "Invalid response when getting mm status", error_size);
		return -5;
	}

	result->is_host = (u8)so_json_get_bool(&doc, "isHost", 0);

	if (so_json_get(&doc, "stages", &stages) == 0) {
		n = so_json_count(&stages);
		for (i = 0; i < n && result->stage_count < SO_MAX_STAGES; i++) {
			long id;
			so_json_at(&stages, i, &el);
			if (so_json_long(&el, &id) == 0)
				result->stages[result->stage_count++] = (u16)id;
		}
	}
	if (result->stage_count == 0) {
		/* Dolphin's fallback; FoD only in singles. */
		for (i = 0; i < (int)(sizeof(default_stages) / sizeof(default_stages[0])); i++)
			result->stages[result->stage_count++] = default_stages[i];
		if (result->player_count == 2)
			result->stages[result->stage_count++] = 0x2;
	}

	result->items = (u32)so_json_get_long(&doc, "items", 0);
	return 0;
}
