/* The Wii port's start of Slippi (wii/src/main.c, before the game's main):
 * what Slippi Nintendont's kernel and loader do before Melee starts.
 *
 * Online: the client starts (so_client_init: the network, the account,
 * Slippi's files and online codeset), Slippi's EXI device answers on slot B
 * (wii/src/exi.c), and the codes installed at boot are the client's own: Slippi's
 * bootloader, which fetches the online codeset from the device and applies
 * it, the delay and rumble buttons on the online character select screen
 * (the delay's shown as the connection instead: online_boot), and no
 * Ranked. The online codeset brings its own gameplay codes (UCF...).
 *
 * Offline: the codes the player chose among Slippi Nintendont's (controller
 * fix, PAL balance, convenience mods, frozen stages, gameplay rules,
 * widescreen), as its loader applies them. */

#include <ogc/cache.h>

#include "so_client.h"
#include "so_console.h"
#include "so_gamelog.h"
#include "so_rollback.h"
#include "so_config.h"
#include "so_files.h"
#include "so_platform.h"
#include "so_wii.h"
#include "string.h"

#define BLOB(name)                                                            \
    extern const u8 melee_gecko_##name[];                                     \
    extern const u32 melee_gecko_##name##_size;
BLOB(online_boot)
BLOB(core)
BLOB(crash_output)
BLOB(screen_wide)
BLOB(screen_wide_shutters)
BLOB(ucf_084)
BLOB(ucf_084_stealth)
BLOB(pal)
BLOB(mods_stealth)
BLOB(mods_tournament)
BLOB(mods_friendlies)
BLOB(stages_stadium)
BLOB(stages_all)
BLOB(gameplay_lgl)
BLOB(gameplay_wobbling)
BLOB(gameplay_both)
BLOB(safety)

/* wii/src/gecko.c, wii/src/exi.c, so_platform_ogc.c */
int melee_wii_gecko_apply(const u8* gct, u32 len);
void melee_wii_exi_slippi(int present);
void so_platform_set_root(const char* root);
void so_replay_enable(int on);

/* what Slippi Launcher's console connection reports as the console's version */
#define MELEE_WII_SO_VERSION "1.0.0"

/* Offline with Slippi's core codes, the codes applied are also listed where
 * Slippi Nintendont keeps its code list: a GCT in Melee's tournament mode
 * code (unused: its menu entry goes to VS mode instead), which the core
 * codes' pointer at 0x80005600 points to. Slippi's recording sends that list
 * with each game (the replay's "gecko codes" event); without it, it reads
 * whatever is there. */
#define CODELIST_BASE 0x801910E0u
#define CODELIST_END 0x8019AF4Cu
#define TOURNAMENT_TO_VS 0x8022D638u

static u32 s_list_at; /* where the next listed code goes (0: not listing) */

void melee_wii_watch_note(u32 addr, u32 len) __attribute__((weak)); /* diag_watch.c, tests */

static void list_write(const void* data, u32 len)
{
    memcpy((void*) s_list_at, data, len);
    if (melee_wii_watch_note != NULL) {
        melee_wii_watch_note(s_list_at, len);
    }
    DCFlushRange((void*) s_list_at, len);
    ICInvalidateRange((void*) s_list_at, len);
    s_list_at += len;
}

static int apply_listed(const u8* codes, u32 len)
{
    int res = melee_wii_gecko_apply(codes, len);
    if (res >= 0 && s_list_at != 0) {
        if (s_list_at + len + 8 > CODELIST_END) {
            so_log("wii: no room in the code list for %u bytes", (unsigned) len);
        } else {
            list_write(codes, len);
        }
    }
    return res;
}

static void list_start(void)
{
    static const u8 header[8] = { 0x00, 0xD0, 0xC0, 0xDE, 0x00, 0xD0, 0xC0, 0xDE };
    static const u32 to_vs = 0x38000002; /* li r0, 2 */
    memcpy((void*) TOURNAMENT_TO_VS, &to_vs, 4);
    if (melee_wii_watch_note != NULL) {
        melee_wii_watch_note(TOURNAMENT_TO_VS, 4);
    }
    DCFlushRange((void*) (TOURNAMENT_TO_VS & ~31u), 32);
    ICInvalidateRange((void*) (TOURNAMENT_TO_VS & ~31u), 32);
    s_list_at = CODELIST_BASE;
    list_write(header, sizeof(header));
}

static void list_end(void)
{
    static const u8 footer[8] = { 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (s_list_at != 0) {
        list_write(footer, sizeof(footer));
        s_list_at = 0;
    }
}

#define APPLY(name) apply_listed(melee_gecko_##name, melee_gecko_##name##_size)

/* The boot codes for online play, with the delay setting on the
 * character select screen turned into a label: the input delay goes with
 * the connection and the opponent's ping (so_client_match_delay; the
 * launcher's setting overrides it), so the label says which connection, and
 * its - and + and the number are blank (spaces: Melee's text wants
 * something to lay out). Its strings, one after the other in its code: the
 * label (16 bytes), "-" and "+" (Shift-JIS, 4 bytes each) and the number's
 * format (4). Pressing where the buttons were changes nothing: the device
 * answers their command with the same delay (so_exi.c, CMD_SET_DELAY). */
static int apply_online_boot(int wifi)
{
    static const u8 strings[28] = { 'D', 'e', 'l', 'a', 'y', ' ', 'F', 'r', 'a', 'm', 'e', 's', 0, 0,
                                    0, 0, 0x81, 0x7C, 0, 0, 0x81, 0x7B, 0, 0, '%', 'd', 0, 0 };
    static u8 codes[8 * 1024] __attribute__((aligned(32)));
    u32 len = melee_gecko_online_boot_size, i;
    if (len > sizeof(codes)) {
        return APPLY(online_boot);
    }
    memcpy(codes, melee_gecko_online_boot, len);
    for (i = 0; i + sizeof(strings) <= len; i += 4) {
        if (memcmp(codes + i, strings, sizeof(strings)) == 0) {
            u8* s = codes + i;
            memset(s, 0, sizeof(strings));
            memcpy(s, wifi ? "WiFi" : "LAN", wifi ? 4 : 3);
            s[16] = s[20] = s[24] = ' ';
            break;
        }
    }
    if (i + sizeof(strings) > len) {
        so_log("wii: the delay setting's label not found: shown as it is");
    }
    return melee_wii_gecko_apply(codes, len);
}

static SoWiiSettings s_copy;
static const SoWiiSettings* s_settings;

const SoWiiSettings* so_wii_settings(void)
{
    return s_settings;
}

int so_wii_rumble(void)
{
    const SoConfig* config;
    if (s_settings == NULL || !s_settings->online) {
        return -1;
    }
    config = so_client_config();
    return config != NULL ? config->rumble != 0 : 1;
}

int so_wii_start(const SoWiiSettings* settings)
{
    s_copy = *settings; /* (the launcher's are gone once it returns) */
    s_settings = settings = &s_copy;
    if (settings->root[0] != '\0') {
        so_platform_set_root(settings->root);
    }
    so_replay_enable(settings->replays);
    so_gamelog_enable(settings->online && settings->diagnostics);
    so_rollback_set(settings->online); /* online play is rollback only */
    if (settings->console && so_console_start(settings->nickname, MELEE_WII_SO_VERSION) < 0) {
        so_log("wii: console mirroring did not start");
    }
    if (settings->online) {
        int res;
        res = so_client_init();
        if (res < 0) {
            so_log("wii: the online client did not start (%d)", res);
            return res;
        }
        /* Slippi's widescreen code with its online codeset, as Slippi
         * Nintendont's kernel adds it (AddOnlineScreenCode) */
        if (settings->screen == SO_WII_SCREEN_WIDE) {
            so_files_gct_add(melee_gecko_screen_wide, melee_gecko_screen_wide_size);
        } else if (settings->screen == SO_WII_SCREEN_WIDE_SHUTTERS) {
            so_files_gct_add(melee_gecko_screen_wide_shutters,
                             melee_gecko_screen_wide_shutters_size);
        }
        melee_wii_exi_slippi(1);
        if (apply_online_boot(so_client_config()->wifi) < 0) {
            return -10;
        }
        /* Tests: codes of one's own (an autoplay codeset, which plays
         * port 1 through the online menus and games), when the file is there */
        {
            static u8 extra[16 * 1024] __attribute__((aligned(32)));
            int n = so_read_file("/slippi/autoplay.gct", extra, sizeof(extra));
            if (n > 0) {
                so_log("wii: applying autoplay.gct (%d bytes)", n);
                melee_wii_gecko_apply(extra, (u32) n);
            }
        }
        return 0;
    }

    /* Offline: Slippi's core codes (its recording, to the device in slot B,
     * as Slippi Nintendont applies them to Melee offline) when replays or
     * console mirroring want the games, then the chosen codes */
    if (settings->replays || settings->console) {
        melee_wii_exi_slippi(1);
        list_start();
        if (APPLY(core) < 0 || APPLY(crash_output) < 0) {
            return -11;
        }
    }
    if (settings->controller_fix == SO_WII_UCF) {
        APPLY(ucf_084);
    } else if (settings->controller_fix == SO_WII_UCF_STEALTH) {
        APPLY(ucf_084_stealth);
    }
    if (settings->pal) {
        APPLY(pal);
    }
    switch (settings->mods) {
    case SO_WII_MODS_STEALTH:
        APPLY(mods_stealth);
        break;
    case SO_WII_MODS_TOURNAMENT:
        APPLY(mods_tournament);
        break;
    case SO_WII_MODS_FRIENDLIES:
        APPLY(mods_friendlies);
        break;
    }
    if (settings->stages == SO_WII_STAGES_STADIUM) {
        APPLY(stages_stadium);
    } else if (settings->stages == SO_WII_STAGES_ALL) {
        APPLY(stages_all);
    }
    switch (settings->gameplay) {
    case SO_WII_GAMEPLAY_LGL:
        APPLY(gameplay_lgl);
        break;
    case SO_WII_GAMEPLAY_WOBBLING:
        APPLY(gameplay_wobbling);
        break;
    case SO_WII_GAMEPLAY_BOTH:
        APPLY(gameplay_both);
        break;
    }
    if (settings->screen == SO_WII_SCREEN_WIDE) {
        APPLY(screen_wide);
    } else if (settings->screen == SO_WII_SCREEN_WIDE_SHUTTERS) {
        APPLY(screen_wide_shutters);
    }
    if (settings->safety) {
        APPLY(safety);
    }
    list_end();
    return 0;
}
