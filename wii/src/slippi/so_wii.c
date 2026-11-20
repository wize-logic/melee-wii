/* The Wii port's start of Slippi (wii/src/main.c, before the game's main):
 * what Slippi Nintendont's kernel and loader do before Melee starts.
 *
 * Online: the client starts (so_client_init: the network, the account,
 * Slippi's files and online codeset), Slippi's EXI device answers on slot B
 * (wii/src/exi.c), and the codes installed at boot are the client's own: Slippi's
 * bootloader, which fetches the online codeset from the device and applies
 * it, the delay setting on the online character select screen (with
 * the launcher's Manual delay only) and the connection's label in place of
 * the rumble switch (online_boot), and no
 * Ranked, Teams or Party (the Online Play screen's options 0, 3 and 4
 * locked: their hook at mn_80229938). The online codeset brings its own gameplay codes (UCF...).
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
#include "so_util.h"
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
BLOB(lag_pd)
BLOB(lag_pdhalfvb)
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

/* wii/src/vi.c: the retrace does PD + half VB's step */
extern int melee_wii_lag_half_vb;

/* Slippi's "PD + half VB" (as Slippi Nintendont's Lag Reduction setting):
 * the game polls the controllers twice a frame (pad.c keeps the rate it
 * asks for), reads them at the first poll and starts its frame at the
 * second, half a frame later, so a frame is shown sooner after its inputs.
 * Two of its codes are not applied here:
 * - its step in the GameCube's retrace handler, which the port replaces
 *   (gecko.c leaves it out): vi.c does that step, as it does 20XX's, which
 *   is the same code;
 * - its hook at the boot's progressive scan question (gm_801A4510), which
 *   puts the game in 480p whatever the cable: the port answers that
 *   question from Wii Settings (vi.c, melee_wii_boot_dtv_status), and 480p
 *   over a composite cable shows nothing. */
static int apply_lag_half_vb(void)
{
    static const u32 progressive_question[] = { 0x801A4570 };
    static u8 gct[8 + 512 + 8] __attribute__((aligned(32)));
    u32 len = melee_gecko_lag_pdhalfvb_size;
    int res;
    if (len + 16 > sizeof(gct)) {
        return -1;
    }
    memset(gct, 0, 8);
    memcpy(gct + 8, melee_gecko_lag_pdhalfvb, len);
    so_put_u32(gct + 8 + len, 0xF0000000);
    so_put_u32(gct + 12 + len, 0);
    len = so_gct_drop(gct, len + 16, progressive_question, 1);
    res = apply_listed(gct + 8, len - 16);
    if (res >= 0) {
        melee_wii_lag_half_vb = 1;
    }
    return res;
}

/* The boot codes for online play, with the character select screen's two
 * rows changed. Slippi's code draws the delay setting (a label, - and +, the
 * number) and, 45 units below it, the rumble switch (a label, On or Off),
 * and takes presses where it drew them, all from one table in its code: the
 * delay row's y (data + 16, data + 20: by the screen's mode), the rumble
 * row's offset from it (data + 116), then their strings. The rows swap
 * places, and:
 * - the rumble row becomes a label saying which connection, with no On or
 *   Off (rumble is the launcher's setting; the device answers the switch's
 *   command with it unchanged: so_exi.c, CMD_SET_RUMBLE);
 * - the delay row, where the rumble switch was, is the delay setting when
 *   the launcher's is Manual (so_client_step_delay), and blank with Auto
 *   (presses there change nothing).
 * Blank strings are spaces: Melee's text wants something to lay out. */
#define BOOT_DELAY_Y_WIDE (-72) /* the table, from the delay's label */
#define BOOT_DELAY_Y (-68)
#define BOOT_RUMBLE_OFFSET 28
#define BOOT_RUMBLE_LABEL 40 /* 8 bytes */
#define BOOT_RUMBLE_ON 48    /* 4 */
#define BOOT_RUMBLE_OFF 52   /* 4 */

static void boot_float(u8* p, float add, int set, float value)
{
    union {
        float f;
        u32 u;
    } v;
    v.u = so_get_u32(p);
    v.f = set ? value : v.f + add;
    so_put_u32(p, v.u);
}

static int apply_online_boot(int wifi, int manual)
{
    /* the delay row's strings, one after the other: the label (16 bytes),
     * "-" and "+" (Shift-JIS, 4 bytes each) and the number's format (4) */
    static const u8 strings[28] = { 'D', 'e', 'l', 'a', 'y', ' ', 'F', 'r', 'a', 'm', 'e', 's', 0, 0,
                                    0, 0, 0x81, 0x7C, 0, 0, 0x81, 0x7B, 0, 0, '%', 'd', 0, 0 };
    static const u8 rumble[16] = { 'R', 'u', 'm', 'b', 'l', 'e', 0, 0, 'O', 'n', 0, 0, 'O', 'f', 'f', 0 };
    static u8 codes[8 * 1024] __attribute__((aligned(32)));
    u32 len = melee_gecko_online_boot_size, i;
    if (len > sizeof(codes)) {
        return APPLY(online_boot);
    }
    memcpy(codes, melee_gecko_online_boot, len);
    for (i = 76; i + BOOT_RUMBLE_OFF + 4 <= len; i += 4) {
        if (memcmp(codes + i, strings, sizeof(strings)) == 0 &&
            memcmp(codes + i + BOOT_RUMBLE_LABEL, rumble, sizeof(rumble)) == 0 &&
            so_get_u32(codes + i + BOOT_RUMBLE_OFFSET) == 0x42340000) { /* 45.0 */
            u8* s = codes + i;
            boot_float(s + BOOT_DELAY_Y_WIDE, 45.0f, 0, 0);
            boot_float(s + BOOT_DELAY_Y, 45.0f, 0, 0);
            boot_float(s + BOOT_RUMBLE_OFFSET, 0, 1, -45.0f);
            if (!manual) {
                memset(s, 0, sizeof(strings));
                s[0] = s[16] = s[20] = s[24] = ' ';
            }
            memset(s + BOOT_RUMBLE_LABEL, 0, sizeof(rumble));
            memcpy(s + BOOT_RUMBLE_LABEL, wifi ? "WiFi" : "LAN", wifi ? 4 : 3);
            s[BOOT_RUMBLE_ON] = s[BOOT_RUMBLE_OFF] = ' ';
            break;
        }
    }
    if (i + BOOT_RUMBLE_OFF + 4 > len) {
        so_log("wii: the character select screen's rows not found: shown as they are");
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
        if (apply_online_boot(so_client_config()->wifi, so_client_config()->delay_override != 0) < 0) {
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
    if (settings->lag == SO_WII_LAG_PD) {
        APPLY(lag_pd);
    } else if (settings->lag == SO_WII_LAG_PD_HALF_VB) {
        apply_lag_half_vb();
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
